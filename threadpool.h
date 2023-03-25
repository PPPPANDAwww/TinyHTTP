#ifndef TRHEADPOOL_H
#define TRHEADPOOL_H
#include <pthread.h>
#include <exception>
#include <stdio.h>
#include <list>
#include "locker.h"

// 线程池类，定义模板类，提高代码的复用性，模板参数T是任务类
template<typename T>
class threadpool{
public:
    threadpool(int m_thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T* request);
private:
    static void* worker(void * arg);
    void run();

private:
    // 线程数量
    int m_thread_number;

    // 线程池数组，大小为m_thread_number
    pthread_t * m_threads;

    // 请求队列中最多允许的，等待处理的请求数量
    int m_max_requests;

    // 请求队列
    std::list< T*> m_workqueue;

    // 互斥锁
    locker m_queuelocker;

    // 信号量，判断是否有任务需要处理
    sem m_queuestat;

    // 是否结束线程
    bool m_stop;

};
template<typename T>
threadpool<T>::threadpool(int m_thread_number, int max_requests):
    m_thread_number(m_thread_number), m_max_requests(max_requests),
    m_stop(false), m_threads(NULL){

        if((m_thread_number <= 0) || (max_requests <= 0)){
            throw std::exception();
        }

        m_threads = new pthread_t[m_thread_number];
        if(!m_threads){
            throw std::exception();
        }

        // 创建thread_number个线程，并将它们设置为线程脱离
        for (int i = 0; i < m_thread_number; i++){
            printf("create the %dth thread\n", i);
            if(pthread_create(m_threads + i, NULL, worker, this) != 0){
                delete [] m_threads;
                throw std::exception();
            }
            
            if(pthread_detach(m_threads[i])){
                delete[] m_threads;
                throw std::exception();                
            }
        }
}

template<typename T>
threadpool<T>::~threadpool(){
    delete[] m_threads;
    m_stop = true;
}

// 添加任务到队列
template<typename T>
bool threadpool<T>::append(T* request){
    m_queuelocker.lock();

    // 如果任务队列中的请求数已经达到了最大数量，则解锁并返回false
    if(m_workqueue.size() > m_max_requests){
        m_queuelocker.unlock();
        return false;
    }

    // 如果可以添加请求，则将请求加入请求队列中，并解锁互斥锁m_queuelocker
    m_workqueue.push_back(request);
    m_queuelocker.unlock();

    // 向信号量m_queuestat发送信号，说明有任务需要处理
    m_queuestat.post();

    return true;
}

template<typename T>
void* threadpool<T>::worker(void * arg){
    threadpool * pool = (threadpool* )arg;
    pool->run();
    return pool;
}

template<typename T>
void threadpool<T>::run(){
    while (!m_stop){
        // 队列中取任务，然后做任务
        m_queuestat.wait();
        m_queuelocker.lock();

        // 如果工作队列为空，解锁
        if(m_workqueue.empty()){
            m_queuelocker.unlock();
            continue;
        }
        
        // 不为空获取工作队列第一个，并解锁
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();

        // 没有取到任务
        if(!request){
            continue;
        }
        request -> process();
    }
}

#endif