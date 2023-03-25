#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>
#include <ctime>
#include <chrono>
#include <iostream>
#include <string>
#include <fstream>
#include <assert.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include "web_timer.h"

#define MAX_FD 65535 // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000 // 最大的一次监听次数

static int pipefd[2]; // 管道文件描述符 0为读 1为写


void addsig(int sig, void(handler)(int)){
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    // 设置临时阻塞信号集
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

// 向管道写数据的信号捕捉回调函数
void sig_to_pipe(int sig){
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}
extern void setnonblocking(int fd);
// 添加文件描述符到epoll
extern void addfd(int epollfd, int fd, bool one_shot, bool et);

// 从epoll中删除文件描述符
extern void removefd(int epollfd, int fd);

// 修改文件描述符
extern void modfd(int epollfd, int fd, int ev);

// log函数
extern void log(std::string str);

int main(int argc, char* argv[]){

    if (argc <= 1){
        printf("按照如下格式运行: %s port_number\n", basename(argv[0]));
        exit(-1);
    }

    // 获取端口号
    int port = atoi(argv[1]);

    // 对SIGPIE信号进行处理,SIGPIE信号进程异常终止
    addsig(SIGPIPE, SIG_IGN);
    
    // 创建线程池，初始化信息 模拟proactor模式
    threadpool<http_conn> * pool = NULL;
    try{
        pool = new threadpool<http_conn>;
    }catch(...){
        exit(-1);
    }
    // 创建一个数组用于保存所有的客户端信息
    http_conn * users = new http_conn[MAX_FD];

    log("Creating socket...\n");
    // 网络socket通信
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if (listenfd < 0){
        log("Socket creation failed.");
        exit(1);
    }
    // assert(listenfd >=0)

    //设置端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // bind绑定
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    log("Binding socket to address and port...\n");
    int ret = bind(listenfd, (struct sockaddr*) &address, sizeof(address));
    if(ret < 0){
        log("Socket binding failed.");
        exit(1);
    }

    // 监听
    log("Listening for incoming connections...\n");
    ret = listen(listenfd, 5);
    if (ret < 0) {
        log("Listening failed.");
        exit(1);
    }

    // 创建epoll对象，事件数组，添加监听的文件描述符
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);

    // 将监听的文件描述符添加到epoll对象中
    addfd(epollfd, listenfd, false, false);
    http_conn::m_epollfd = epollfd;
    assert(epollfd != -1);

    // 创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false, false); // epoll检测管道

    // 设置信号处理函数
    addsig(SIGALRM, sig_to_pipe); // 定时器信号
    addsig(SIGTERM, sig_to_pipe); // SIGTERM 关闭服务器
    bool stop_server = false;   // 关闭服务器标志位

    // 定时器设置
    bool timeout = false; // 定时器周期已到
    alarm(TIMESLOT); //定时产生SIGALARM信号

    // 循环检测有无事件发生
    while(true){
        log("Waiting for events...\n");
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1); // 检测到了几个事件
        if((num < 0) && (errno != EINTR)){
            log("Epoll wait failure");
            printf("epoll failure\n");
            break;
        }

        // 循环遍历事件数组
        for(int i = 0; i < num; i++){
            int sockfd = events[i].data.fd;
            log("Event detected on sockfd\n");

            // 有客户端连接进来连接
            if(sockfd == listenfd){  // 监听文件描述符的事件响应
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlen);
                log("client connected!\n");
                if (connfd < 0){
                    printf("error is: %d\n", errno);
                    continue;
                }

                if(http_conn:: m_user_count >= MAX_FD){
                    // 目前的连接数满了
                    // 给客户端写一个信息：服务器内部正忙
                    log("m_user fulled!\n");
                    close(connfd);
                    continue;
                }

                // 将新的客户的数据初始化，放到数组中
                users[connfd].init(connfd, client_address);

            }else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                // 对方异常断开或者错误等事件
                // 关闭连接
                users[sockfd].close_conn();
                
                // 读管道有数据，SIGALRM或者SIGTERM信号触发
            }else if(sockfd == pipefd[0] && (events[i].events & EPOLLIN)){
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if(ret == -1){
                    continue;
                }else if(ret == 0){
                    continue;
                }else{
                    for (int i = 0; i < ret; ++i){
                        switch(signals[i]){
                            case SIGALRM:
                                timeout = true;
                                break;
                            case SIGTERM:
                                stop_server = true;
                        }
                    }
                }
            }else if(events[i].events & EPOLLIN){
                log("read event happen!\n");
                // 是否有读的事件发生
                if(users[sockfd].read()){
                    // 一次性把所有数据都读完
                    pool->append(users + sockfd);
                    log("reading all data...\n");
                    log("*************************\n");
                }else{
                    users[sockfd].close_conn();
                }
            }else if(events[i].events & EPOLLOUT){
                // 是否有写的事件发生
                log("write event happen!\n");
                if(!users[sockfd].write()){
                    // 一次性写完所有数据
                    log("writing all data...\n");
                    users[sockfd].close_conn();
                }
            }else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                // 对方异常断开或错误等事件
                users[sockfd].close_conn();
                http_conn::m_timer_lst.del_timer(users[sockfd].timer);
            }
        }
        // 最后处理定时事件，因为IO事件有更高的优先级，虽然这样定时任务不能精准按照预定时间进行
        if(timeout){
            // 定时处理任务，实际上就是调用tick()函数
            http_conn::m_timer_lst.tick();

            // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
            alarm(TIMESLOT);
            timeout = false;  // 重置timeout
        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete [] users;
    delete pool;

    return 0;
}