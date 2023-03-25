#include "http_conn.h"
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <sys/uio.h>
#include <iostream>
#include <ctime>
#include <chrono>
#include <string>
#include <fstream>

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

const char* doc_root = "/home/panda/Desktop/TinyHttp/resource";

int http_conn::m_epollfd = -1; 
int http_conn::m_user_count = 0;
int http_conn::m_request_cnt = 0;
sort_timer_lst http_conn::m_timer_lst;

// log函数
void log(std::string message){
    // 获取当前时间
    time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    // 打开log文件
    std::ofstream logfile;
    logfile.open("server.log", std::ios::out | std::ios::app);

    // 写入error数据
    if (logfile.is_open()){
        logfile << "[" << ctime(&now) << "]" << message << std::endl;
        logfile.close();
    }else{
        std::cerr << "Unable to open log file" << std::endl;
    }
}
// 设置文件描述符非阻塞
void setnonblocking(int fd){
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);
}

// 添加文件描述符到epoll
void addfd(int epollfd, int fd, bool one_shot, bool et){
    epoll_event event;
    event.data.fd = fd;
    if(et){
        event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
    }else{
        event.events = EPOLLIN | EPOLLRDHUP; // 默认水平触发
    }

    if(one_shot){
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置文件描述符非阻塞
    setnonblocking(fd);
}

// 从epoll中删除文件描述符
void removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改文件描述符，重置socket上EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev){
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP | EPOLLET;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 初始化新接收的连接
void http_conn::init(int sockfd, const sockaddr_in &addr){
    m_sockfd = sockfd;
    m_address = addr;
    
    // 端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 添加到epoll对象中
    addfd(m_epollfd, m_sockfd, true, ET);
    m_user_count++;
    init();

    // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表m_timer_lst
    util_timer* new_timer = new util_timer;
    new_timer->user_data = this;
    time_t curr_time = time(NULL);
    new_timer -> expire = curr_time + 3 * TIMESLOT;
    this -> timer = new_timer;
    m_timer_lst.add_timer(new_timer);
}

// 关闭连接
void http_conn::close_conn(){
    if(m_sockfd != -1){
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

void http_conn::init(){
    bzero(m_read_buf, READ_BUFFER_SIZE);  // 读缓冲区
    m_read_idx = 0;                     // 标识读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置
    m_checked_idx = 0;                  // 当前正在分析的字符在读缓冲区中的位置
    m_start_line = 0;                   // 当前正在解析的行的起始位置

    m_check_state = CHECK_STATE_REQUESTLINE;          // 主状态机当前所处的状态
    m_method = GET;                    // 请求方法

    bzero(m_real_file, FILENAME_LEN);     // 客户请求的目标文件的完整路径= doc_root + m_url
    m_url = 0;                        // 客户请求目标文件的文件名
    m_version= 0;                    // HTTP协议版本号
    m_host = 0;                       // 主机名
    m_content_length = 0;               // HTTP请求的消息总长度
    m_linger = false;                      // HTTP请求是否要求保持连接

    bzero(m_write_buf, WRITE_BUFFER_SIZE);// 写缓冲区
    m_write_idx = 0;                    // 写缓冲区中待发送的字节数
    bytes_to_send = 0;                  // 将要发送的数据字节数
    bytes_have_send = 0;                // 已经发送的字节数
}
bool http_conn::read(){
    if (timer){
        time_t curr_time = time(NULL);
        timer->expire = curr_time + 3 * TIMESLOT;
        m_timer_lst.adjust_timer(timer);
    }

    // 超出缓冲区大小
    if(m_read_idx >= READ_BUFFER_SIZE){
        return false;
    }
    int bytes_read = 0;

    // 一次性全部读进来
    while(true){
        // 从m_read_buf + m_read_idx索引处开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if(bytes_read == -1){
            if( errno == EAGAIN || errno == EWOULDBLOCK){
                break;  // 非阻塞读取，没有数据了
            }
            return false; // 读取错误，调用conn_close()
        }else if(bytes_read == 0){  // 对方关闭连接，调用conn_close()
            return false;
        }
        m_read_idx += bytes_read;   // 更新下一次读取位置
        
    }
    ++m_request_cnt;
    return true;
}

// 根据\r\n解析一行数据
http_conn::LINE_STATUS http_conn::parse_line(){
    char temp = '\0';
    for(; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1 == m_read_idx))
            {
                return LINE_OPEN;
            }
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if ((m_checked_idx > 1) && (m_read_buf[m_checked_idx - 1] == '\r'))
            {
                m_read_buf[m_read_idx - 1] = '\0';
                m_read_buf[m_read_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//  解析HTTP请求行，获得请求方法，目标URL，HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text){
    // Check if text is valid
    if (text == NULL || strlen(text) == 0) {
        log("Invalid request line\n");
        return BAD_REQUEST;
    }

    // Find the URL
    m_url = strpbrk(text, " \t");
    if (!m_url) {
        log("Invalid URL\n");
        return BAD_REQUEST;
    }

    // Terminate the method string
    *m_url++ = '\0';

    // Check if method is valid
    char* method = text;
    if (strcasecmp(method, "GET") == 0){
        method = 0;
    }else{
        log("Unsupported method\n");
        return BAD_REQUEST;
    }

    // Find the HTTP version
    m_url += strspn(m_url, " \t"); // 不存在的第一个下标
    m_version = strpbrk(m_url, " \t");  // 存在的第一个下标
    if (!m_version){
        log("Invalid HTTP version\n");
        return BAD_REQUEST;
    }

    // Terminate the URL string
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");

    // Check if HTTP version is valid
    if (strcasecmp(m_version, "HTTP/1.1") != 0 ) {
        if (strcasecmp(m_version, "HTTP/1.0") != 0 ){
            log(m_version);
            log("m_version burn");
            return BAD_REQUEST;
        }
        m_linger = false;
    }
    if (strncasecmp(m_url, "http://", 7) == 0 ) {   
        m_url += 7;
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        m_url = strchr( m_url, '/' );
    }
    if ( !m_url || m_url[0] != '/' ) {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER; // 检查状态变成检查头
    return NO_REQUEST;
}    


//  解析HTTP请求头信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text){
    // 遇到空行，表示头部字段解析完毕
    if (text[0] == '\0'){
        if (m_content_length != 0){
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    }else if (strncasecmp(text, "Connection:", 11) == 0){
        // Connection: keep-alive
        text += 11;
        text += strspn(text, "\t"); // 找到第一个不等于这个字符/str的位置
        if(strcasecmp(text, "keep-alive") == 0){
            m_linger = true;
        }
    }else if (strncasecmp(text, "Content-Length:", 15) == 0){
        // 处理Content-Length头部字段
        text += 15;
        text += strspn(text, "\t"); // 找到第一个不等于这个字符/str的位置
        m_content_length = atol(text); //ascii to long
    }else if (strncasecmp(text, "Host:", 5) == 0){
        // 处理Content-Length头部字段
        text += 5;
        text += strspn(text, "\t"); // 找到第一个不等于这个字符/str的位置
        m_host = text; //ascii to long
    }else{
        printf("unknown header %s\n", text);
    }
    return NO_REQUEST;
}

// 没有完全解析HTTP请求体，只是判断其是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content(char* text){
    if(m_read_idx >= (m_content_length + m_checked_idx)){
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}
char* http_conn::get_line(){
    return m_read_buf + m_start_line;
}

// 主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read(){
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = nullptr;
    while(((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) || ((line_status = parse_line()) == LINE_OK)){
        // 获取一行数据
        text = get_line();
        m_start_line = m_checked_idx;
        // printf("Got 1 http line: %s\n", text);

        // 主状态机
        switch(m_check_state){
            case CHECK_STATE_REQUESTLINE:{
                ret = parse_request_line(text);
                if(ret == BAD_REQUEST){
                    log("parse_request_line burn!\n");
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:{
                ret = parse_headers(text);
                if(ret == BAD_REQUEST){
                    log("parse_headers burn!\n");
                    return BAD_REQUEST;
                }else if(ret == GET_REQUEST){
                    log("get request!");
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:{
                ret = parse_content(text);
                if(ret == GET_REQUEST){
                    log("get request2!");
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default:{
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

// 得到一个完整正确的HTTP请求之后，分析需要获取文件的属性
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其映射到内存地址m_file_address处
// 告诉调用者获取成功
http_conn::HTTP_CODE http_conn::do_request(){

    strcpy(m_real_file, doc_root); //把doc_root复制到m_real_file里
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN -len - 1);  // 相当于把url加到doc_root后面

    //  获取m_real_file文件的相关状态信息，-1失败，0成功
    if (stat(m_real_file, &m_file_stat) < 0){
        return NO_RESOURCE;
    }

    // 判断访问权限
    if(!(m_file_stat.st_mode & S_IROTH)){
        return FORBIDDEN_RERQUEST;
    }

    // 判断是否是目录
    if (S_ISDIR(m_file_stat.st_mode)){
        log("m_file is dir\n");
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open(m_real_file, O_RDONLY);
    // 创建内存映射
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    printf("file requested!\n");

    return FILE_REQUEST;
}

// 对内存映射去执行munmap操作
void http_conn::unmap(){
    if(m_file_address){
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = nullptr;
    }
}

// 写HTTP响应
bool http_conn::write(){
    int temp = 0;

    if (bytes_to_send == 0){
        // 将要发送的字节为0，这一次响应结束，改为可读
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }
    while(1){
        // writev将多个数据存储在一起，将驻留在两个或更多的不连接的缓冲区中的数据一次写出去。
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp <= -1){
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件
            // 此时，服务器无法立刻接受同一客户的下一个请求，但可以保证连接的完整性
            if (errno == EAGAIN){
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        // 如果当前已经发送的字节数大于等于第一个iovec的长度
        if (bytes_have_send >= m_iv[0].iov_len){
            // 把第一个iovec的长度置为0，因为已经发送完了
            m_iv[0].iov_len = 0;
            
            // 把第二个iovec的base指针设为文件地址加上已发送的字节数减去响应头的长度
            // 这样第二个iovec就指向了响应体
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else{
        // 如果当前已经发送的字节数小于第一个iovec的长度，说明响应头还没有发送完
        // 把第一个iovec的base指针设为写缓冲区加上已经发送的字节数
        // 这样第一个iovec仍然指向了响应头，只是指针往后移了
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }
        if (bytes_to_send <= 0){
            // 没有数据要发送了，改为可读，等待下一次事件
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            // 如果支持HTTP长连接，需要调用init()函数，初始化HTTP对象
            if(m_linger){
                init();
                return true;
            }else{
                return false;
            }
        }

    }
}

// 往写中写入待发送的数据缓冲
bool http_conn::add_response(const char* format, ...){
    if(m_write_idx >= WRITE_BUFFER_SIZE){
        return false;
    }
    // 定义一个可变参数列表
    va_list arg_list;

    // 初始化可变参数列表，有format组可变参数
    va_start(arg_list, format);

    // 将格式化的数据从变量参数列表写入大小已设置的缓冲区
    // - str：把生成的格式化的字符串存放在这里
    // - size：str可接受的最大字符数
    // - format：指定输出格式的字符串，它决定了你需要提供的可变参数的类型、个数和顺序
    // - ap：va_list变量
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if(len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) return false;

    m_write_idx += len;
    va_end(arg_list);
    return true;
}
bool http_conn::add_status_line(int status, const char* title){
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

void http_conn::add_headers(int content_len){
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len){
    return add_response("Content-Length: %d\r\n", "HTTP/1.1", content_len);
}

bool http_conn::add_content_type(){
    return add_response("Content-Type: %s\r\n", "text/html");
}

bool http_conn::add_linger(){
    return add_response("Connection: %s\r\n", (m_linger == true)? "keep-alive":"close");
}

bool http_conn::add_blank_line(){
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char* content){
    return add_response("%s", content);
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret){
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form)){
                return false;
            }
            log("Response code is INTERNAL_ERROR\n");
            break;
        case BAD_REQUEST:
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if(!add_content(error_400_form)){
                return false;
            }
            log("Response code is BAD_REQUEST\n");
            break;
        case NO_RESOURCE:
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if(!add_content(error_404_form)){
                return false;
            }
            log("Response code is NO_RESOURCE\n");
            break;
        case FORBIDDEN_RERQUEST:
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if(!add_content(error_403_form)){
                return false;
            }
            log("Response code is FORBIDDEN_RERQUEST\n");
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title);
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;

            bytes_to_send = m_write_idx + m_file_stat.st_size; 
            log("Response code is FILE_REQUEST\n");        
            return true;
        default:
            return false;
    }

    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

// 由线程池中的工作线程调用，处理HTTP请求的入口函数
void http_conn::process(){
    HTTP_CODE read_ret = process_read();
    
    if (read_ret == BAD_REQUEST){
        log("process_read = BAD_REQUEST");
    }

    // No request表示请求不完整，需要继续接收请求数据
    if (read_ret == NO_REQUEST){
        //注册并监听读事件
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    // 调用process_write完成报文响应
    bool write_ret = process_write(read_ret);
    log("answer over!\n");
    if (!write_ret){
        close_conn();
    }
    //注册并监听写事件
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}
