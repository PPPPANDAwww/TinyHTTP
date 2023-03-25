#ifndef WEB_TIMER_H
#define WEB_TIMER_H

#include <stdio.h>
#include <time.h>
#include <arpa/inet.h>
#include "locker.h"
#include "http_conn.h"

class http_conn;

//  定时器类
class util_timer{
public:
    util_timer(): prev(NULL), next(NULL){}

public:
    time_t expire; // 任务超时时间，绝对时间
    http_conn* user_data;
    util_timer* prev;
    util_timer* next;
};

// 定时器链表，升序双向链表，带有头节点和尾结点
class sort_timer_lst{
public:
    sort_timer_lst(): head(NULL), tail(NULL){}
    ~sort_timer_lst(){
        util_timer* tmp = head;
        while(tmp){
            head = head->next;
            delete tmp;
            tmp = head;
        }
    }

    // 添加到链表中
    void add_timer(util_timer* timer);

    // 当某个定时任务发生变化时，调整对应的定时器在链表中的位置
    // 只考虑超时时间延长的情况，即该定时器需要往链表的尾部移动
    void adjust_timer(util_timer* timer);

    // 将目标定时器从链表中删除
    void del_timer(util_timer* timer);

    // SIGALARM信号每次被触发就在其信号处理函数中执行一次tick函数
    // 以处理链表上到期的任务
    void tick();

private:
    //  重载辅助函数，被公有的add_timer函数和adjust_timer
    // 函数调用，将目标定时器timer添加到节点lst_head之后的部分链表中
    void add_timer(util_timer* timer, util_timer* lst_head);

private:
    util_timer* head;
    util_timer* tail;
};

#endif