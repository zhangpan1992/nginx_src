
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_EVENT_PIPE_H_INCLUDED_
#define _NGX_EVENT_PIPE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


typedef struct ngx_event_pipe_s  ngx_event_pipe_t;

//处理接收自上游的包体的回调方法原型
typedef ngx_int_t (*ngx_event_pipe_input_filter_pt)(ngx_event_pipe_t *p,
                                                    ngx_buf_t *buf);

//向下游发送响应的回调方法原型
typedef ngx_int_t (*ngx_event_pipe_output_filter_pt)(void *data,
                                                     ngx_chain_t *chain);

/*
*ngx_event_pipe_t结构体是上游优先转发方式的核心结构体
*它必须要由HTTP模块创建，维护着上下游间转发的响应包体
*旨在解决: nginx从不会把相同的内容复制到两块内存中，因此而引出的
*同一块内存可能既要接收上游发来的响应，又要准备向下游发送，还可能要写入临时文件中的矛盾
*/ 
struct ngx_event_pipe_s {

	//Nginx与上游服务器间的连接
    ngx_connection_t  *upstream;

	//Nginx与下游客户端间的连接
    ngx_connection_t  *downstream;

	//直接接收自上游服务器的缓冲区链表，仅在接收响应时使用
    ngx_chain_t       *free_raw_bufs;
	
	//接收到的上游响应缓冲区
    ngx_chain_t       *in;

	//指向刚刚接收到的一个缓冲区
    ngx_chain_t      **last_in;

	//保存着将要发送给客户端的缓冲区链表
    ngx_chain_t       *out;

	//待释放的缓冲区
    ngx_chain_t       *free;
    ngx_chain_t       *busy;

    /*
     * the input filter i.e. that moves HTTP/1.1 chunks
     * from the raw bufs to an incoming chain
     */

    ngx_event_pipe_input_filter_pt    input_filter;
    void                             *input_ctx;

    ngx_event_pipe_output_filter_pt   output_filter;
    void                             *output_ctx;

    unsigned           read:1;
    unsigned           cacheable:1;
    unsigned           single_buf:1;
    unsigned           free_bufs:1;
    unsigned           upstream_done:1;
    unsigned           upstream_error:1;
    unsigned           upstream_eof:1;
    unsigned           upstream_blocked:1;
    unsigned           downstream_done:1;
    unsigned           downstream_error:1;
    unsigned           cyclic_temp_file:1;

    ngx_int_t          allocated;
    ngx_bufs_t         bufs;
    ngx_buf_tag_t      tag;

    ssize_t            busy_size;

    off_t              read_length;
    off_t              length;

    off_t              max_temp_file_size;
    ssize_t            temp_file_write_size;

    ngx_msec_t         read_timeout;
    ngx_msec_t         send_timeout;
    ssize_t            send_lowat;

    ngx_pool_t        *pool;
    ngx_log_t         *log;

    ngx_chain_t       *preread_bufs;
    size_t             preread_size;
    ngx_buf_t         *buf_to_file;

    ngx_temp_file_t   *temp_file;

    /* STUB */ int     num;
};


ngx_int_t ngx_event_pipe(ngx_event_pipe_t *p, ngx_int_t do_write);
ngx_int_t ngx_event_pipe_copy_input_filter(ngx_event_pipe_t *p, ngx_buf_t *buf);
ngx_int_t ngx_event_pipe_add_free_buf(ngx_event_pipe_t *p, ngx_buf_t *b);


#endif /* _NGX_EVENT_PIPE_H_INCLUDED_ */
