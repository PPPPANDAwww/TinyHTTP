#include "web_timer.h"

// 添加到链表中
void sort_timer_lst::add_timer(util_timer* timer){
    if (!time){
        return;
    }
    if(!head){ // 添加为第一个节点
        head = tail = timer;
    }
    // 目标定时器的超时时间最少，则把该定时器插入链表头部，作为链表新的头结点
    else if(timer -> expire < head -> expire){
        timer -> next = head;
        head -> prev = timer;
        head = timer;
    }
    else{
        // 调用重载函数，插入head节点之后适合的位置，保证链表升序
        add_timer(timer, head);
    }
}

// 当某个定时任务发生变化时，调整对应的定时器在链表中的位置
// 只考虑超时时间延长的情况，即该定时器需要往链表的尾部移动
void sort_timer_lst::adjust_timer(util_timer* timer){
    if(!time){
        return;
    }
    util_timer* temp = timer -> next;

    // 如果被调整的目标定时器在链表的尾部
    // 或者该定时器新的超时时间值仍然小于下一个
    if(!temp || timer->expire < temp -> expire){
        return;
    }

    // 如果目标定时器是头节点，则将该定时器从链表中取出并重新插入链表
    else if(timer == head){
        head = head -> next;
        head -> prev = NULL;
        timer -> next = NULL;
        add_timer(timer, head);
    }
    else{
        // 如果目标定时器不是链表的头节点，则将定时器从链表取出，插入原来所在位置
        timer -> prev -> next = timer -> next;
        timer -> next -> prev = timer -> prev;
        add_timer(timer, timer -> next);
    }

}

/* 一个重载的辅助函数，它被公有的 add_timer 函数和 adjust_timer 函数调用
该函数表示将目标定时器 timer 添加到节点 lst_head 之后的部分链表中 */
void sort_timer_lst::add_timer(util_timer* timer, util_timer* lst_head){
    util_timer* prev = lst_head;
    util_timer* temp = prev -> next;

    // 循环遍历lst_head之后的节点，找到后一个超时时间大于timer的节点插入
    while(temp){
        if(timer -> expire < temp -> expire){
            temp -> prev -> next = timer;
            timer -> prev = temp ->prev;
            timer -> next = temp;
            temp -> prev = timer;
            break;
        }
        prev = temp;
        temp = temp -> next;
    }

    // 遍历完lst_head节点之后的部分链表，仍未找到则当作尾结点插入
    if (!temp){
        timer -> next = NULL;
        prev -> next = timer;
        timer -> prev = prev;
        tail = timer;
    }

}

// 将目标定时器从链表中删除
void sort_timer_lst::del_timer(util_timer* timer){
    if(!time){
        return;
    }
        // 1. 链表中只有一个定时器即目标定时器
    if((timer == head) && (timer == tail )){
        delete timer;
        head = NULL;
        tail = NULL;
    }else if(timer == head){
        // 2. 链表中至少有2个定时器，目标定时器是头节点
        head = head -> next;
        head -> prev = NULL;
        delete timer;
    }else if (timer == tail){
        // 3. 链表中至少有2个定时器，目标定时器是尾节点
        tail = timer -> prev;
        tail -> next = NULL;
        delete timer;
    }else{
        // 4. 链表中至少有2个定时器，目标定时器是中间节点
        timer -> prev -> next = timer -> next;
        timer -> next -> prev = timer -> prev;
        delete timer;
    }
}

// SIGALARM信号每次被触发就在其信号处理函数中执行一次tick函数
// 以处理链表上到期的任务：断掉超时的连接，删除定时器
void sort_timer_lst::tick(){
    if (!time){
        return;
    }

    time_t curr_time = time(NULL);
    util_timer* temp = head;
    // 从头节点依次处理每个定时器，直到遇到一个尚未到期的定时器

    while(temp){
        // 当前未超时
        if(curr_time < temp -> expire){
            break;
        }

        // 调用定时器的回调函数，以执行定时任务，关闭连接
        temp -> user_data -> close_conn();
        
        // 删除定时器
        del_timer(temp);
        temp = head;
    }
}