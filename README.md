# TinyWebServer
## 虚拟机：Vmware fushion pro
## 运行系统：ubuntu 20.04
## 使用语言：c++
## 功能：
1. 利用 Socket 来实现不同主机之间的通信； 
2. 利用 epoll 技术实现 I/O多路复用，支持 ET和 LT 两种触发方式，可以同时监听多个请求； 
3. 基于线程池处理数据 +阻塞队列实现单 Reactor 多线程模型，增加并行服务数量； 
4. 使用有限状态机解析 HTTP 请求报文，对 GET和 POST 报文进行处理
5. 具有定时器以及超时检测功能
6. 添加了log方便debug
7. 待添加：压测。。。
