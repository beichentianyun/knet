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

#ifndef LOOP_BALANCER_API_H
#define LOOP_BALANCER_API_H

#include "config.h"

/*
 * 创建负载均衡器
 * @return loop_balancer_t实例
 */
loop_balancer_t* loop_balancer_create();

/*
 * 销毁负载均衡器
 * @param loop_balancer_t实例
 */
void loop_balancer_destroy(loop_balancer_t* balancer);

/*
 * 添加事件循环到负载均衡器
 * @param loop_balancer_t实例
 * @param loop loop_t实例
 * @retval error_ok 成功
 * @retval 其他 失败
 */
int loop_balancer_attach(loop_balancer_t* balancer, loop_t* loop);

/*
 * 从负载均衡器内删除事件循环
 * @param loop_balancer_t实例
 * @param loop loop_t实例
 * @retval error_ok 成功
 * @retval 其他 失败
 */
int loop_balancer_detach(loop_balancer_t* balancer, loop_t* loop);

#endif /* LOOP_BALANCER_API_H */
