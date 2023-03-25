# TinyWebServer
## 环境：Vmware fushion pro
## 运行系统：ubuntu 20.04
## 使用语言：c++
## 功能：
• 利用 Socket 来实现不同主机之间的通信； 
• 利用 epoll 技术实现 I/O多路复用，支持 ET和 LT 两种触发方式，可以同时监听多个请求； 
• 基于线程池处理数据 +阻塞队列实现单 Reactor 多线程模型，增加并行服务数量； 
• 使用有限状态机解析 HTTP 请求报文，对 GET和 POST 报文进行处理
• 具有定时器以及超时检测功能
• 添加了log方便debug
