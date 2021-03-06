/*
 * Copyright (c) 2014-2015, dennis wang
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL dennis wang BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "loop.h"
#include "list.h"
#include "channel_ref.h"
#include "channel.h"
#include "misc.h"
#include "loop_balancer.h"
#include "stream.h"

struct _loop_t {
    dlist_t*         active_channel_list; /* 活跃管道链表 */
    dlist_t*         close_channel_list;  /* 已关闭管道链表 */
    dlist_t*         event_list;          /* 事件链表 */
    lock_t*          lock;                /* 锁-事件链表*/
    channel_ref_t*   notify_channel;      /* 事件通知写管道 */
    channel_ref_t*   read_channel;        /* 事件通知读管道 */
    loop_balancer_t* balancer;            /* 负载均衡器 */
    void*            impl;                /* 事件选取器实现 */
    volatile int     running;             /* 事件循环运行标志 */
    thread_id_t      thread_id;           /* 事件选取器当前运行线程ID */
};

typedef enum _loop_event_e {
    loop_event_accept = 1,  /* 接受新连接事件 */
    loop_event_send,        /* 发送事件 */
    loop_event_close,       /* 关闭事件 */
} loop_event_e;

typedef struct _loop_event_t {
    channel_ref_t* channel_ref; /* 事件相关管道 */
    buffer_t*      send_buffer; /* 发送缓冲区指针 */
    loop_event_e   event;       /* 事件类型 */
} loop_event_t;

loop_event_t* loop_event_create(channel_ref_t* channel_ref, buffer_t* send_buffer, loop_event_e e) {
    loop_event_t* event = create(loop_event_t);
    assert(event);
    event->channel_ref = channel_ref;
    event->send_buffer = send_buffer;
    event->event = e;
    return event;
}

void loop_event_destroy(loop_event_t* loop_event) {
    assert(loop_event);
    destroy(loop_event);
}

channel_ref_t* loop_event_get_channel_ref(loop_event_t* loop_event) {
    assert(loop_event);
    return loop_event->channel_ref;
}

buffer_t* loop_event_get_send_buffer(loop_event_t* loop_event) {
    assert(loop_event);
    return loop_event->send_buffer;
}

loop_event_e loop_event_get_event(loop_event_t* loop_event) {
    assert(loop_event);
    return loop_event->event;
}

loop_t* loop_create() {
    socket_t pair[2] = {0}; /* 事件读写描述符 */
    loop_t*  loop    = create(loop_t);
    assert(loop);
    memset(loop, 0, sizeof(loop_t));
    /* 建立选取器实现 */
    if (impl_create(loop)) {
        destroy(loop);
        return 0;
    }
    /* 建立读写描述符 */
    if (socket_pair(pair)) {
        destroy(loop);
        return 0;
    }
    loop->active_channel_list = dlist_create();
    loop->close_channel_list = dlist_create();
    loop->event_list = dlist_create();
    loop->lock = lock_create();
    loop->notify_channel = loop_create_channel_exist_socket_fd(loop, pair[0], 0, 0);
    assert(loop->notify_channel);
    loop->read_channel = loop_create_channel_exist_socket_fd(loop, pair[1], 0, 1024 * 64);
    assert(loop->read_channel);
    loop_add_channel_ref(loop, loop->notify_channel);
    loop_add_channel_ref(loop, loop->read_channel);
    channel_ref_set_state(loop->notify_channel, channel_state_active);
    channel_ref_set_state(loop->read_channel, channel_state_active);
    /* 注册读事件 */
    channel_ref_set_event(loop->read_channel, channel_event_recv);
    /* 设置读事件回调 */
    channel_ref_set_cb(loop->read_channel, loop_queue_cb);
    return loop;
}

void loop_destroy(loop_t* loop) {
    dlist_node_t*  node        = 0;
    dlist_node_t*  temp        = 0;
    channel_ref_t* channel_ref = 0;
    loop_event_t*  event       = 0;
    /* 关闭管道 */
    dlist_for_each_safe(loop->active_channel_list, node, temp) {
        channel_ref = (channel_ref_t*)dlist_node_get_data(node);
        channel_ref_update_close_in_loop(channel_ref_get_loop(channel_ref), channel_ref);
    }
    /* 销毁选取器 */
    impl_destroy(loop);
    /* 销毁已关闭管道 */
    dlist_for_each_safe(loop->close_channel_list, node, temp) {
        channel_ref = (channel_ref_t*)dlist_node_get_data(node);
        channel_ref_destroy(channel_ref);
    }
    dlist_destroy(loop->close_channel_list);
    dlist_destroy(loop->active_channel_list);
    /* 销毁未处理事件 */
    dlist_for_each_safe(loop->event_list, node, temp) {
        event = (loop_event_t*)dlist_node_get_data(node);
        destroy(event);
    }
    dlist_destroy(loop->event_list);
    lock_destroy(loop->lock);
    destroy(loop);
}

void loop_add_event(loop_t* loop, loop_event_t* loop_event) {
    assert(loop);
    assert(loop_event);
    lock_lock(loop->lock);
    /* 事件添加到链表尾部 */
    dlist_add_tail_node(loop->event_list, loop_event);
    lock_unlock(loop->lock);
    loop_notify(loop);
}

void loop_notify_accept(loop_t* loop, channel_ref_t* channel_ref) {
    assert(loop);
    assert(channel_ref);
    loop_add_event(loop, loop_event_create(channel_ref, 0, loop_event_accept));
}

void loop_notify_send(loop_t* loop, channel_ref_t* channel_ref, buffer_t* send_buffer) {
    assert(loop);
    assert(channel_ref);
    assert(send_buffer);
    loop_add_event(loop, loop_event_create(channel_ref, send_buffer, loop_event_send));
}

void loop_notify_close(loop_t* loop, channel_ref_t* channel_ref) {
    assert(loop);
    assert(channel_ref);
    loop_add_event(loop, loop_event_create(channel_ref, 0, loop_event_close));
}

void loop_queue_cb(channel_ref_t* channel, channel_cb_event_e e) {
    if (e & channel_cb_event_recv) {
        /* 清空所有读到的数据 */
        stream_eat(channel_ref_get_stream(channel));
        loop_event_process(channel_ref_get_loop(channel));
    } else if (e & channel_cb_event_close) {
        if (loop_check_running(channel_ref_get_loop(channel))) {
            /* 连接断开, 致命错误 */
            assert(0);
        }
    }
}

void loop_notify(loop_t* loop) {
    char c = 1;
    assert(loop);
    /* 发送一个字节触发读回调  */
    socket_send(channel_ref_get_socket_fd(loop->notify_channel), &c, sizeof(c));
}

void loop_event_process(loop_t* loop) {
    dlist_node_t* node       = 0;
    dlist_node_t* temp       = 0;
    loop_event_t* loop_event = 0;
    assert(loop);
    lock_lock(loop->lock);
    /* 每次读事件回调内处理整个事件链表 */
    dlist_for_each_safe(loop->event_list, node, temp) {
        loop_event = (loop_event_t*)dlist_node_get_data(node);
        switch(loop_event->event) {
            case loop_event_accept:
                channel_ref_update_accept_in_loop(loop, loop_event->channel_ref);
                break;
            case loop_event_send:
                channel_ref_update_send_in_loop(loop, loop_event->channel_ref, loop_event->send_buffer);
                break;
            case loop_event_close:
                channel_ref_update_close_in_loop(loop, loop_event->channel_ref);
                break;
            default:
                break;
        }
        loop_event_destroy(loop_event);
        dlist_delete(loop->event_list, node);
    }
    lock_unlock(loop->lock);
}

channel_ref_t* loop_create_channel_exist_socket_fd(loop_t* loop, socket_t socket_fd, uint32_t max_send_list_len, uint32_t recv_ring_len) {
    assert(loop);
    return channel_ref_create(loop, channel_create_exist_socket_fd(socket_fd, max_send_list_len, recv_ring_len));
}

channel_ref_t* loop_create_channel(loop_t* loop, uint32_t max_send_list_len, uint32_t recv_ring_len) {
    assert(loop);
    return channel_ref_create(loop, channel_create(max_send_list_len, recv_ring_len));
}

thread_id_t loop_get_thread_id(loop_t* loop) {
    assert(loop);
    return loop->thread_id;
}

int loop_run_once(loop_t* loop) {
    assert(loop);
    loop->thread_id = thread_get_self_id();
    return impl_run_once(loop);
}

int loop_run(loop_t* loop) {
    int error = 0;
    assert(loop);
    loop->running = 1;
    while (loop->running) {
        error = loop_run_once(loop);
        if (error_loop_fail == error) {
            loop->running = 0;
        }
    }
    return error;
}

void loop_exit(loop_t* loop) {
    assert(loop);
    loop->running = 0;
}

dlist_t* loop_get_active_list(loop_t* loop) {
    assert(loop);
    return loop->active_channel_list;
}

dlist_t* loop_get_close_list(loop_t* loop) {
    assert(loop);
    return loop->close_channel_list;
}

void loop_add_channel_ref(loop_t* loop, channel_ref_t* channel_ref) {
    dlist_node_t* node = channel_ref_get_loop_node(channel_ref);
    assert(loop);
    assert(channel_ref);
    if (node) {
        /* 已经加入过链表，不需要创建链表节点 */
        dlist_add_front(loop->active_channel_list, node);
    } else {
        /* 创建链表节点 */
        dlist_add_front_node(loop->active_channel_list, channel_ref);
    }
    /* 设置节点 */
    channel_ref_set_loop_node(channel_ref, dlist_get_front(loop->active_channel_list));
    /* 通知选取器添加管道 */
    impl_add_channel_ref(loop, channel_ref);
}

void loop_remove_channel_ref(loop_t* loop, channel_ref_t* channel_ref) {
    assert(loop);
    assert(channel_ref);
    /* 解除与当前链表关联，但不销毁节点 */
    dlist_remove(loop->active_channel_list, channel_ref_get_loop_node(channel_ref));
}

void loop_set_impl(loop_t* loop, void* impl) {
    assert(loop);
    assert(impl);
    loop->impl = impl;
}

void* loop_get_impl(loop_t* loop) {
    assert(loop);
    return loop->impl;
}

void loop_close_channel_ref(loop_t* loop, channel_ref_t* channel_ref) {
    assert(loop);
    assert(channel_ref);
    /* 在外层函数内套接字已经被关闭 */
    /* 从活跃链表内取出 */
    loop_remove_channel_ref(loop, channel_ref);
    /* 加入到已关闭链表 */
    dlist_add_front(loop->close_channel_list, channel_ref_get_loop_node(channel_ref));
}

void loop_set_balancer(loop_t* loop, loop_balancer_t* balancer) {
    assert(loop);
    assert(balancer);
    loop->balancer = balancer;
}

loop_balancer_t* loop_get_balancer(loop_t* loop) {
    assert(loop);
    return loop->balancer;
}

void loop_check_timeout(loop_t* loop, time_t ts) {
    dlist_node_t*  node        = 0;
    dlist_node_t*  temp        = 0;
    channel_ref_t* channel_ref = 0;
    dlist_for_each_safe(loop_get_active_list(loop), node, temp) {
        channel_ref = (channel_ref_t*)dlist_node_get_data(node);
        if (channel_ref_check_connect_timeout(channel_ref, ts)) {
            /* 连接超时 */            
            if (channel_ref_get_cb(channel_ref)) {
                channel_ref_get_cb(channel_ref)(channel_ref, channel_cb_event_connect_timeout);
            }
        }
        if (channel_ref_check_timeout(channel_ref, ts)) {
            /* 读超时，心跳 */
            if (channel_ref_get_cb(channel_ref)) {
                channel_ref_get_cb(channel_ref)(channel_ref, channel_cb_event_timeout);
            }
        }
    }
}

void loop_check_close(loop_t* loop) {
    dlist_node_t*  node        = 0;
    dlist_node_t*  temp        = 0;
    channel_ref_t* channel_ref = 0;
    dlist_for_each_safe(loop_get_close_list(loop), node, temp) {
        channel_ref = (channel_ref_t*)dlist_node_get_data(node);
        if (error_ok == channel_ref_destroy(channel_ref)) {
            dlist_delete(loop_get_close_list(loop), node);
        }
    }
}

int loop_check_running(loop_t* loop) {
    return loop->running;
}
