#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>
#include "web_timer.h"

class sort_timer_lst;
class util_timer;

#define COUT_OPEN 1
const bool ET = true;
#define TIMESLOT 5   // 定时器周期：秒

class http_conn{
public:
    static int m_epollfd; // 所有的socket上的事件都被注册到同一个epoll对象中，所以设置成静态
    static int m_user_count; // 统计用户的数量
    static int m_request_cnt; // 接收到的请求次数
    static sort_timer_lst m_timer_lst; // 定时器链表

    static const int FILENAME_LEN = 200; // 文件名的最大长度
    static const int READ_BUFFER_SIZE = 2048; // 读缓冲区的大小
    static const int WRITE_BUFFER_SIZE = 1024; // 写缓冲区的大小

    util_timer* timer; // 定时器

public:
    // HTTP请求方法
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};
    /*
    解析客户端请求时，主状态机的状态
    CHECK_STATE_REQUESTLINE：当前正在分析请求行
    CHECK_STATE_HEADER：分析请求头
    CHECK_STATE_CONTENT：分析请求体
    */
   enum CHECK_STATE {CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT};
    
    /*
    服务器处理HTTP请求的可能结果，报文解析的结果
    NO_REQUEST：请求不完整，需要继续读取客户数据
    GET_REQUEST：表示获得了一个完成的客户数据
    BAD_REQUEST：表示客户请求应答错误
    NO_RESOURCE：表示服务器没有资源
    FORBIDDEN_RERQUEST：表示客户对资源没有足够的访问权限
    FILE_REQUEST：文件请求，获取文件成功
    INTERNAL_ERROR：表示服务器内部错误
    CLOSED_CONNECTION：表示客户端已经关闭连接了
    */ 
    enum HTTP_CODE {NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_RERQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION};
    
    // 行的读取状态，0-读取到一个完整的行 1-行出错 2- 行数据尚且不完整
    enum LINE_STATUS {LINE_OK = 0, LINE_BAD, LINE_OPEN};

 public:   
    http_conn(){};
    ~http_conn(){};

    void process(); // 响应，处理客户端的请求
    void init(int sockfd, const sockaddr_in &addr); // 初始化新接收的连接
    void close_conn(); // 关闭连接
    bool read(); // 非阻塞读数据
    bool write(); // 非阻塞写数据

private:
    void init(); // 初始化连接
    HTTP_CODE process_read(); // 解析HTTP请求
    bool process_write(HTTP_CODE ret); // 填充HTTP应答

    // 下面这组函数被process_read调用以分析HTTP请求
    HTTP_CODE parse_request_line(char* text);
    HTTP_CODE parse_headers(char* text);
    HTTP_CODE parse_content(char* text);
    HTTP_CODE do_request();
    char* get_line();
    LINE_STATUS parse_line();

    // 下面这组函数被process_write调用以填充HTTP应答
    void unmap();
    bool add_response(const char* format, ...);
    bool add_content(const char* content);
    bool add_content_type();
    bool add_status_line(int status, const char* title);
    void add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

private:
    int m_sockfd; // 该HTTP连接的socket
    sockaddr_in m_address; // 通信的socket地址

private:
    char m_read_buf[READ_BUFFER_SIZE];  // 读缓冲区
    int m_read_idx;                     // 标识读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置
    int m_checked_idx;                  // 当前正在分析的字符在读缓冲区中的位置
    int m_start_line;                   // 当前正在解析的行的起始位置

    CHECK_STATE m_check_state;          // 主状态机当前所处的状态
    METHOD m_method;                    // 请求方法

    char m_real_file[FILENAME_LEN];     // 客户请求的目标文件的完整路径= doc_root + m_url
    char* m_url;                        // 客户请求目标文件的文件名
    char* m_version;                    // HTTP协议版本号
    char* m_host;                       // 主机名
    int m_content_length;               // HTTP请求的消息总长度
    bool m_linger;                      // HTTP请求是否要求保持连接

    char m_write_buf[WRITE_BUFFER_SIZE];// 写缓冲区
    int m_write_idx;                    // 写缓冲区中待发送的字节数
    char* m_file_address;               // 客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;            // 目标文件的状态，判断文件是否存在，是否为目录，是否可读，文件大小
    struct iovec m_iv[2];               // 采用writev来执行写操作
    int m_iv_count;                     // 被写内存块的数量
    int bytes_to_send;                  // 将要发送的数据字节数
    int bytes_have_send;                // 已经发送的字节数
};

#endif