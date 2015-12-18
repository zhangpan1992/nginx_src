
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_event_pipe.h>


static ngx_int_t ngx_event_pipe_read_upstream(ngx_event_pipe_t *p);
static ngx_int_t ngx_event_pipe_write_to_downstream(ngx_event_pipe_t *p);

static ngx_int_t ngx_event_pipe_write_chain_to_temp_file(ngx_event_pipe_t *p);
static ngx_inline void ngx_event_pipe_remove_shadow_links(ngx_buf_t *buf);
static ngx_int_t ngx_event_pipe_drain_chains(ngx_event_pipe_t *p);



/*
*该方法被[ ngx_http_upstream_process_upstream ](处理上游读事件的回调方法)
*和 [ ngx_http_upstream_process_downstream ] (处理下游写事件的回调方法)方法调用
*该方法实现了缓存转发功能，通过调用ngx_event_pipe_read_upstream和ngx_event_pipe_write_to_downstream
*/

ngx_int_t
ngx_event_pipe(ngx_event_pipe_t *p, ngx_int_t do_write)
{
    u_int         flags;
    ngx_int_t     rc;
    ngx_event_t  *rev, *wev;

    for ( ;; ) {
        if (do_write) {
            p->log->action = "sending to client";

			//发送响应包体
            rc = ngx_event_pipe_write_to_downstream(p); 

            if (rc == NGX_ABORT) {
                return NGX_ABORT;
            }

            if (rc == NGX_BUSY) {
                return NGX_OK;
            }
        }

		//无料可读了
        p->read = 0;
		//阻塞缓冲区(为1时)，使之可读不可写，以释放缓存的空间
        p->upstream_blocked = 0;

        p->log->action = "reading upstream";

		//读取上游响应
        if (ngx_event_pipe_read_upstream(p) == NGX_ABORT) {
            return NGX_ABORT;
        }

		//read标志位会在ngx_event_pipe_read_upstream方法中置为1
		//如果阻塞了(upstream_blocked=1)，即使无料可读，也不能退出循环(if不成立)
        if (!p->read && !p->upstream_blocked) {
            break;
        }

		//读完了又可写了
        do_write = 1;
    }

    if (p->upstream->fd != -1) {
        rev = p->upstream->read;

        flags = (rev->eof || rev->error) ? NGX_CLOSE_EVENT : 0;

        if (ngx_handle_read_event(rev, flags) != NGX_OK) {
            return NGX_ABORT;
        }

        if (rev->active && !rev->ready) {
            ngx_add_timer(rev, p->read_timeout);

        } else if (rev->timer_set) {
            ngx_del_timer(rev);
        }
    }

    if (p->downstream->fd != -1 && p->downstream->data == p->output_ctx) {
        wev = p->downstream->write;
        if (ngx_handle_write_event(wev, p->send_lowat) != NGX_OK) {
            return NGX_ABORT;
        }

        if (!wev->delayed) {
            if (wev->active && !wev->ready) {
                ngx_add_timer(wev, p->send_timeout);

            } else if (wev->timer_set) {
                ngx_del_timer(wev);
            }
        }
    }

    return NGX_OK;
}


/*

ngx_event_pipe_read_upstream函数完成下面几个功能：

0.从preread_bufs，free_raw_bufs或者ngx_create_temp_buf寻找一块空闲的或[部分空闲]的内存；
1.调用p->upstream->recv_chain==ngx_readv_chain，用writev的方式读取FCGI的数据,填充chain。
2.对于整块buf都满了的chain[节点](而不是整个chain)调用input_filter(ngx_http_fastcgi_input_filter)进行upstream协议解析，
  比如FCGI协议，解析后的结果放入p->in里面；
3.对于没有填充满的buffer节点，放入free_raw_bufs以待下次进入时从后面进行追加。
4.当然了，如果对端发送完数据FIN了，那就直接调用input_filter处理free_raw_bufs这块数据

*/

static ngx_int_t
ngx_event_pipe_read_upstream(ngx_event_pipe_t *p)
{
    ssize_t       n, size;
    ngx_int_t     rc;
    ngx_buf_t    *b;
    ngx_chain_t  *chain, *cl, *ln;

	/*upstream_eof表示与上游的连接结束
    *upstream_error表示上游响应超时或调用recv出现错误
    *upstream_done表示与上游交互完成，可在input_filter方法中设置 
    */
    if (p->upstream_eof || p->upstream_error || p->upstream_done) {
        return NGX_OK;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, p->log, 0,
                   "pipe read upstream: %d", p->upstream->read->ready);

    for ( ;; ) {

        if (p->upstream_eof || p->upstream_error || p->upstream_done) {
            break;
        }


		//同时满足，read 结束
        if (p->preread_bufs == NULL && !p->upstream->read->ready) {
            break;
        }

        if (p->preread_bufs) {

            /* use the pre-read bufs if they exist */

			//拷贝预读缓冲区的内容(其实仅仅是指针拷贝)
            chain = p->preread_bufs;

			//可以看到设置preread_bufs为空，这样子，下次循环，则会进入另外的处理，也就是需要从upstream读取数据
            p->preread_bufs = NULL;
            n = p->preread_size;

            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, p->log, 0,
                           "pipe preread: %z", n);

            if (n) {
                p->read = 1;
            }


		//preread_bufs预读缓冲区为空
		
        } else {

#if (NGX_HAVE_KQUEUE)

            /*
             * kqueue notifies about the end of file or a pending error.
             * This test allows not to allocate a buf on these conditions
             * and not to call c->recv_chain().
             */

            if (p->upstream->read->available == 0
                && p->upstream->read->pending_eof)
            {
                p->upstream->read->ready = 0;
                p->upstream->read->eof = 1;
                p->upstream_eof = 1;
                p->read = 1;

                if (p->upstream->read->kq_errno) {
                    p->upstream->read->error = 1;
                    p->upstream_error = 1;
                    p->upstream_eof = 0;

                    ngx_log_error(NGX_LOG_ERR, p->log,
                                  p->upstream->read->kq_errno,
                                  "kevent() reported that upstream "
                                  "closed connection");
                }

                break;
            }
#endif

			//free_raw_bufs用来表示一次调用ngx_event_pipe_read_upstream过程中接收到的上游响应
            if (p->free_raw_bufs) {

                /* use the free bufs if they exist */

                chain = p->free_raw_bufs;

				//single_buf位为1时，表示接收上游响应 一次只能接收一个 ngx_buf_t 缓冲区
				
                if (p->single_buf) {

					//将新接收到的缓冲区(一个 ngx_buf_t 缓冲区)置到free_raw_bufs链表的最后
                    p->free_raw_bufs = p->free_raw_bufs->next;
                    chain->next = NULL;
                } else {
                    p->free_raw_bufs = NULL;
                }

			//free_raw_bufs为空，分配新的缓冲区接收上游响应，之后调用recv_chain方法接收上游响应包体
            } else if (p->allocated < p->bufs.num) {

                /* allocate a new buf if it's still allowed */

                b = ngx_create_temp_buf(p->pool, p->bufs.size);
                if (b == NULL) {
                    return NGX_ABORT;
                }

                p->allocated++;

                chain = ngx_alloc_chain_link(p->pool);
                if (chain == NULL) {
                    return NGX_ABORT;
                }

                chain->buf = b;
                chain->next = NULL;

            } else if (!p->cacheable
                       && p->downstream->data == p->output_ctx
                       && p->downstream->write->ready
                       && !p->downstream->write->delayed)
            {
                /*
                 * if the bufs are not needed to be saved in a cache and
                 * a downstream is ready then write the bufs to a downstream
                 */

                p->upstream_blocked = 1;

                ngx_log_debug0(NGX_LOG_DEBUG_EVENT, p->log, 0,
                               "pipe downstream ready");

                break;

            } else if (p->cacheable
                       || p->temp_file->offset < p->max_temp_file_size)
            {

                /*
                 * if it is allowed, then save some bufs from r->in
                 * to a temporary file, and add them to a r->out chain
                 */
                 
				//写buf到临时文件。而所写的buf就是p->in,也就是将要发送给client的数据
                rc = ngx_event_pipe_write_chain_to_temp_file(p);

                ngx_log_debug1(NGX_LOG_DEBUG_EVENT, p->log, 0,
                               "pipe temp offset: %O", p->temp_file->offset);

                if (rc == NGX_BUSY) {
                    break;
                }

                if (rc == NGX_AGAIN) {
                    if (ngx_event_flags & NGX_USE_LEVEL_EVENT
                        && p->upstream->read->active
                        && p->upstream->read->ready)
                    {
                        if (ngx_del_event(p->upstream->read, NGX_READ_EVENT, 0)
                            == NGX_ERROR)
                        {
                            return NGX_ABORT;
                        }
                    }
                }

                if (rc != NGX_OK) {
                    return rc;
                }

                chain = p->free_raw_bufs;
                if (p->single_buf) {
                    p->free_raw_bufs = p->free_raw_bufs->next;
                    chain->next = NULL;
                } else {
                    p->free_raw_bufs = NULL;
                }

            } else {

                /* there are no bufs to read in */

                ngx_log_debug0(NGX_LOG_DEBUG_EVENT, p->log, 0,
                               "no pipe bufs to read in");

                break;
            }

			//调用recv_chain方法获取上游响应,填充chain
			//这里的chain是不是只有一块? 其next成员为空呢，不一定????
            n = p->upstream->recv_chain(p->upstream, chain);

            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, p->log, 0,
                           "pipe recv chain: %z", n);

			//将chain添加到free_raw_bufs的开头
            if (p->free_raw_bufs) {
                chain->next = p->free_raw_bufs;
            }

            p->free_raw_bufs = chain;

            if (n == NGX_ERROR) {
                p->upstream_error = 1;
                return NGX_ERROR;
            }

            if (n == NGX_AGAIN) {
                if (p->single_buf) {
					//将这块缓冲区的shadow域释放掉，因为刚刚接收到的缓冲区，必然不存在多次引用的情况
					//所以shadow成员要指向空指针
                    ngx_event_pipe_remove_shadow_links(chain->buf);
                }

                break;
            }

			//设置read，表示已经读取了数据
            p->read = 1;

            if (n == 0) {
                p->upstream_eof = 1;
                break;
            }
        }

		
		//for循环的最后一段，到达这一段说明从后端的数据已经读取到了chain中(不管preread_bufs是否为空)
		//n为已经读取的数据，于是开始遍历已经读取的chain
        p->read_length += n;
        cl = chain;

		//chain已经是链表的头部了，等于free_raw_bufs所以下面可以置空先
        p->free_raw_bufs = NULL;

		/*遍历待处理缓冲区中链表中的ngx_buf_t
		*如果还有链表数据并且长度不为0，也就是这次的还没有处理完。那如果之前保留有一部分数据(预读数据)呢?
        *不会的，如果之前预读了数据，那么上面的大if语句else里面进不去，就是此时的n肯定等于preread_bufs的长度preread_size。
        *如果之前没有预读数据，但free_raw_bufs不为空，那也没关系，free_raw_bufs里面的数据肯定已经在下面几行处理过了。
		*/
        while (cl && n > 0) {

            ngx_event_pipe_remove_shadow_links(cl->buf);

            size = cl->buf->end - cl->buf->last;


			/*读取到的内容大于等于当前缓冲区ngx_buf_t的大小
			* 对于整块buf都满了的chain节点调用input_filter进行upstream协议解析,解析后的结果放入p->in里面；
			*即，在in链表中添加cl->buf缓冲区
			*/
            if (n >= size) {
                cl->buf->last = cl->buf->end;

                /* STUB */ cl->buf->num = p->num++;

                if (p->input_filter(p, cl->buf) == NGX_ERROR) {
                    return NGX_ABORT;
                }

                n -= size;
                ln = cl;
                cl = cl->next;
                ngx_free_chain(p->pool, ln);


			//如果这个节点的空闲内存数目(size)大于剩下要处理的，就将剩下的存放在这里。
            //啥意思? 不用调用 input_filter 了吗，不是。是这样的，如果剩下的这块数据还不够塞满当前这个cl的缓存大小，
            //那就先存起来，怎么存呢: 别释放cl了，只是移动其大小，然后n=0使循环退出。
            } else {
                cl->buf->last += n;
                n = 0;
            }
        }


		//读取到的内容小于当前缓冲区ngx_buf_t的大小，将free_raw_bufs指向剩余缓冲区
		//如果cl还存在，则说明我们开始设置的chain，只有一部分被使用了，因此此时将这些chain保存到free_raw_bufs中。
		//可以看到如果chain只有一部分被使用，然后当再次循环，则使用的chain会直接使用free_raw_bufs,也就是我们前一次没有使用完全的chain
		
        if (cl) {
            for (ln = cl; ln->next; ln = ln->next) { /* void */ }

            ln->next = p->free_raw_bufs;

			//这样free_raw_bufs就非空，下次循环可以直接使用
            p->free_raw_bufs = cl;
        }
    }

#if (NGX_DEBUG)

    for (cl = p->busy; cl; cl = cl->next) {
        ngx_log_debug8(NGX_LOG_DEBUG_EVENT, p->log, 0,
                       "pipe buf busy s:%d t:%d f:%d "
                       "%p, pos %p, size: %z "
                       "file: %O, size: %z",
                       (cl->buf->shadow ? 1 : 0),
                       cl->buf->temporary, cl->buf->in_file,
                       cl->buf->start, cl->buf->pos,
                       cl->buf->last - cl->buf->pos,
                       cl->buf->file_pos,
                       cl->buf->file_last - cl->buf->file_pos);
    }

    for (cl = p->out; cl; cl = cl->next) {
        ngx_log_debug8(NGX_LOG_DEBUG_EVENT, p->log, 0,
                       "pipe buf out  s:%d t:%d f:%d "
                       "%p, pos %p, size: %z "
                       "file: %O, size: %z",
                       (cl->buf->shadow ? 1 : 0),
                       cl->buf->temporary, cl->buf->in_file,
                       cl->buf->start, cl->buf->pos,
                       cl->buf->last - cl->buf->pos,
                       cl->buf->file_pos,
                       cl->buf->file_last - cl->buf->file_pos);
    }

    for (cl = p->in; cl; cl = cl->next) {
        ngx_log_debug8(NGX_LOG_DEBUG_EVENT, p->log, 0,
                       "pipe buf in   s:%d t:%d f:%d "
                       "%p, pos %p, size: %z "
                       "file: %O, size: %z",
                       (cl->buf->shadow ? 1 : 0),
                       cl->buf->temporary, cl->buf->in_file,
                       cl->buf->start, cl->buf->pos,
                       cl->buf->last - cl->buf->pos,
                       cl->buf->file_pos,
                       cl->buf->file_last - cl->buf->file_pos);
    }

    for (cl = p->free_raw_bufs; cl; cl = cl->next) {
        ngx_log_debug8(NGX_LOG_DEBUG_EVENT, p->log, 0,
                       "pipe buf free s:%d t:%d f:%d "
                       "%p, pos %p, size: %z "
                       "file: %O, size: %z",
                       (cl->buf->shadow ? 1 : 0),
                       cl->buf->temporary, cl->buf->in_file,
                       cl->buf->start, cl->buf->pos,
                       cl->buf->last - cl->buf->pos,
                       cl->buf->file_pos,
                       cl->buf->file_last - cl->buf->file_pos);
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, p->log, 0,
                   "pipe length: %O", p->length);

#endif



	/*
	*上游连接结束
	*/
    if (p->free_raw_bufs && p->length != -1) {
        cl = p->free_raw_bufs;

        if (cl->buf->last - cl->buf->pos >= p->length) {

            p->free_raw_bufs = cl->next;

            /* STUB */ cl->buf->num = p->num++;

            if (p->input_filter(p, cl->buf) == NGX_ERROR) {
                 return NGX_ABORT;
            }

            ngx_free_chain(p->pool, cl);
        }
    }

    if (p->length == 0) {
        p->upstream_done = 1;
        p->read = 1;
    }


	//处理最后一个剩余的缓冲区
    if ((p->upstream_eof || p->upstream_error) && p->free_raw_bufs) {

        /* STUB */ p->free_raw_bufs->buf->num = p->num++;

		//这次只处理可能剩余的最后一个缓冲区(ngx_buf_t)
        if (p->input_filter(p, p->free_raw_bufs->buf) == NGX_ERROR) {
            return NGX_ABORT;
        }

        p->free_raw_bufs = p->free_raw_bufs->next;

        if (p->free_bufs && p->buf_to_file == NULL) {
            for (cl = p->free_raw_bufs; cl; cl = cl->next) {
                if (cl->buf->shadow == NULL) {
                    ngx_pfree(p->pool, cl->buf->start);
                }
            }
        }
    }

    if (p->cacheable && p->in) {
        if (ngx_event_pipe_write_chain_to_temp_file(p) == NGX_ABORT) {
            return NGX_ABORT;
        }
    }

    return NGX_OK;
}



/*
*nginx会将不同状态的缓冲区链入不同链表上
*与发送相关的缓冲区链表有三：busy链表，out链表，in链表
*in链表表示刚刚接收到的上游响应
*  在ngx_event_pipe_read_downstream方法中调用input_filter方法将刚刚接收到的缓冲区设置到in链表中
*out链表保存着将要发送给客户端的缓冲区链表:在写入临时文件成功时，会把in链表中写入文件的那部分缓冲区添加到out链表中
*busy链表存储的是上次调用ngx_http_output_filter过程中没有发出的部分，这个链表中的缓冲区已经保存到out链表中
 它仅用于记录还有多大的响应正等待发送

*该方法就负责把out链表，in链表, busy链表中管理的缓冲区发送给下游客户端
*out链表中的缓冲区内容在响应中的位置要比in链表更靠前,所以out需要优先发送给下游
*代码中有一个大循环，在与下游的连接事件上处于可写状态时
*尽可能地循环发送out和in链表缓冲区中的内容
*/


static ngx_int_t
ngx_event_pipe_write_to_downstream(ngx_event_pipe_t *p)
{
    u_char            *prev;
    size_t             bsize;
    ngx_int_t          rc;
    ngx_uint_t         flush, flushed, prev_last_shadow;
    ngx_chain_t       *out, **ll, *cl, file;
    ngx_connection_t  *downstream;

    downstream = p->downstream;

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, p->log, 0,
                   "pipe write downstream: %d", downstream->write->ready);

    flushed = 0;

    for ( ;; ) {
        if (p->downstream_error) {
            return ngx_event_pipe_drain_chains(p);
        }

        if (p->upstream_eof || p->upstream_error || p->upstream_done) {

            /* pass the p->out and p->in chains to the output filter */

            for (cl = p->busy; cl; cl = cl->next) {
                cl->buf->recycled = 0;
            }

			//把out链表中的缓冲区发送到下游客户端
            if (p->out) {
                ngx_log_debug0(NGX_LOG_DEBUG_EVENT, p->log, 0,
                               "pipe write downstream flush out");

                for (cl = p->out; cl; cl = cl->next) {
                    cl->buf->recycled = 0;
                }

                rc = p->output_filter(p->output_ctx, p->out);

                if (rc == NGX_ERROR) {
                    p->downstream_error = 1;
                    return ngx_event_pipe_drain_chains(p);
                }

                p->out = NULL;
            }

			//把in链表中的缓冲区发送到下游客户端
            if (p->in) {
                ngx_log_debug0(NGX_LOG_DEBUG_EVENT, p->log, 0,
                               "pipe write downstream flush in");

                for (cl = p->in; cl; cl = cl->next) {
                    cl->buf->recycled = 0;
                }

                rc = p->output_filter(p->output_ctx, p->in);

                if (rc == NGX_ERROR) {
                    p->downstream_error = 1;
                    return ngx_event_pipe_drain_chains(p);
                }

                p->in = NULL;
            }

            if (p->cacheable && p->buf_to_file) {

                file.buf = p->buf_to_file;
                file.next = NULL;

                if (ngx_write_chain_to_temp_file(p->temp_file, &file)
                    == NGX_ERROR)
                {
                    return NGX_ABORT;
                }
            }

            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, p->log, 0,
                           "pipe write downstream done");

            /* TODO: free unused bufs */

			//置1，ngx_event_pipe_write_to_downstream方法结束
            p->downstream_done = 1;
            break;
        }

        if (downstream->data != p->output_ctx
            || !downstream->write->ready
            || downstream->write->delayed)
        {
            break;
        }

        /* bsize is the size of the busy recycled bufs */

        prev = NULL;
        bsize = 0;

        for (cl = p->busy; cl; cl = cl->next) {

            if (cl->buf->recycled) {
                if (prev == cl->buf->start) {
                    continue;
                }

                bsize += cl->buf->end - cl->buf->start;
                prev = cl->buf->start;
            }
        }

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, p->log, 0,
                       "pipe write busy: %uz", bsize);

        out = NULL;


		//如果busy buffers链表上的缓冲区数量已经足够，那么无需再从out和in链表上去取了，直接flush 
        if (bsize >= (size_t) p->busy_size) {
            flush = 1;
            goto flush;
        }

        flush = 0;
        ll = NULL;
        prev_last_shadow = 1;

        for ( ;; ) {
            if (p->out) {
                cl = p->out;

                if (cl->buf->recycled) {
                    ngx_log_error(NGX_LOG_ALERT, p->log, 0,
                                  "recycled buffer in pipe out chain");
                }

                p->out = p->out->next;

            } else if (!p->cacheable && p->in) {
                cl = p->in;

                ngx_log_debug3(NGX_LOG_DEBUG_EVENT, p->log, 0,
                               "pipe write buf ls:%d %p %z",
                               cl->buf->last_shadow,
                               cl->buf->pos,
                               cl->buf->last - cl->buf->pos);

                if (cl->buf->recycled && prev_last_shadow) {
                    if (bsize + cl->buf->end - cl->buf->start > p->busy_size) {
                        flush = 1;
                        break;
                    }

                    bsize += cl->buf->end - cl->buf->start;
                }

                prev_last_shadow = cl->buf->last_shadow;

                p->in = p->in->next;

            } else {
                break;
            }


			/*
			*实际上是通过ll和cl把out和in都串起来，最后统一发送out链表
			*ll = &cl->next;的作用就是串起来cl
			*
			*/
            cl->next = NULL;

            if (out) {
                *ll = cl;
            } else {
            	//更新out(原out链表后面接着的就是in链表中的缓冲区了)
                out = cl;
            }
            ll = &cl->next;
        }

    flush:

        ngx_log_debug2(NGX_LOG_DEBUG_EVENT, p->log, 0,
                       "pipe write: out:%p, f:%d", out, flush);

        if (out == NULL) {

            if (!flush) {
                break;
            }

            /* a workaround for AIO */
            if (flushed++ > 10) {
                return NGX_BUSY;
            }
        }

		//实际上，out与busy指向的是同一缓冲区链表，out发完了，busy指向的缓冲区自然也就变空了
		//如果out链表中的缓冲区没有发完，就会让busy指向那块没发出的缓冲区
        rc = p->output_filter(p->output_ctx, out);

		//out中没发出的缓冲区加到busy缓冲区，发出的加入到free待释放缓冲区链表中
        ngx_chain_update_chains(p->pool, &p->free, &p->busy, &out, p->tag);

        if (rc == NGX_ERROR) {
            p->downstream_error = 1;
            return ngx_event_pipe_drain_chains(p);
        }

        for (cl = p->free; cl; cl = cl->next) {

            if (cl->buf->temp_file) {
                if (p->cacheable || !p->cyclic_temp_file) {
                    continue;
                }

                /* reset p->temp_offset if all bufs had been sent */

                if (cl->buf->file_last == p->temp_file->offset) {
                    p->temp_file->offset = 0;
                }
            }

            /* TODO: free buf if p->free_bufs && upstream done */

            /* add the free shadow raw buf to p->free_raw_bufs */

            if (cl->buf->last_shadow) {
                if (ngx_event_pipe_add_free_buf(p, cl->buf->shadow) != NGX_OK) {
                    return NGX_ABORT;
                }

                cl->buf->last_shadow = 0;
            }

            cl->buf->shadow = NULL;
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_event_pipe_write_chain_to_temp_file(ngx_event_pipe_t *p)
{
    ssize_t       size, bsize, n;
    ngx_buf_t    *b;
    ngx_uint_t    prev_last_shadow;
    ngx_chain_t  *cl, *tl, *next, *out, **ll, **last_out, **last_free, fl;

    if (p->buf_to_file) {
        fl.buf = p->buf_to_file;
        fl.next = p->in;
        out = &fl;

    } else {
        out = p->in;
    }

    if (!p->cacheable) {

        size = 0;
        cl = out;
        ll = NULL;
        prev_last_shadow = 1;

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, p->log, 0,
                       "pipe offset: %O", p->temp_file->offset);

        do {
            bsize = cl->buf->last - cl->buf->pos;

            ngx_log_debug4(NGX_LOG_DEBUG_EVENT, p->log, 0,
                           "pipe buf ls:%d %p, pos %p, size: %z",
                           cl->buf->last_shadow, cl->buf->start,
                           cl->buf->pos, bsize);

            if (prev_last_shadow
                && ((size + bsize > p->temp_file_write_size)
                    || (p->temp_file->offset + size + bsize
                        > p->max_temp_file_size)))
            {
                break;
            }

            prev_last_shadow = cl->buf->last_shadow;

            size += bsize;
            ll = &cl->next;
            cl = cl->next;

        } while (cl);

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, p->log, 0, "size: %z", size);

        if (ll == NULL) {
            return NGX_BUSY;
        }

        if (cl) {
           p->in = cl;
           *ll = NULL;

        } else {
           p->in = NULL;
           p->last_in = &p->in;
        }

    } else {
        p->in = NULL;
        p->last_in = &p->in;
    }

    n = ngx_write_chain_to_temp_file(p->temp_file, out);

    if (n == NGX_ERROR) {
        return NGX_ABORT;
    }

    if (p->buf_to_file) {
        p->temp_file->offset = p->buf_to_file->last - p->buf_to_file->pos;
        n -= p->buf_to_file->last - p->buf_to_file->pos;
        p->buf_to_file = NULL;
        out = out->next;
    }

    if (n > 0) {
        /* update previous buffer or add new buffer */

        if (p->out) {
            for (cl = p->out; cl->next; cl = cl->next) { /* void */ }

            b = cl->buf;

            if (b->file_last == p->temp_file->offset) {
                p->temp_file->offset += n;
                b->file_last = p->temp_file->offset;
                goto free;
            }

            last_out = &cl->next;

        } else {
            last_out = &p->out;
        }

        cl = ngx_chain_get_free_buf(p->pool, &p->free);
        if (cl == NULL) {
            return NGX_ABORT;
        }

        b = cl->buf;

        ngx_memzero(b, sizeof(ngx_buf_t));

        b->tag = p->tag;

        b->file = &p->temp_file->file;
        b->file_pos = p->temp_file->offset;
        p->temp_file->offset += n;
        b->file_last = p->temp_file->offset;

        b->in_file = 1;
        b->temp_file = 1;

        *last_out = cl;
    }

free:

    for (last_free = &p->free_raw_bufs;
         *last_free != NULL;
         last_free = &(*last_free)->next)
    {
        /* void */
    }

    for (cl = out; cl; cl = next) {
        next = cl->next;

        cl->next = p->free;
        p->free = cl;

        b = cl->buf;

        if (b->last_shadow) {

            tl = ngx_alloc_chain_link(p->pool);
            if (tl == NULL) {
                return NGX_ABORT;
            }

            tl->buf = b->shadow;
            tl->next = NULL;

            *last_free = tl;
            last_free = &tl->next;

            b->shadow->pos = b->shadow->start;
            b->shadow->last = b->shadow->start;

            ngx_event_pipe_remove_shadow_links(b->shadow);
        }
    }

    return NGX_OK;
}


/* the copy input filter */

ngx_int_t
ngx_event_pipe_copy_input_filter(ngx_event_pipe_t *p, ngx_buf_t *buf)
{
    ngx_buf_t    *b;
    ngx_chain_t  *cl;

    if (buf->pos == buf->last) {
        return NGX_OK;
    }

    if (p->free) {
        cl = p->free;
        b = cl->buf;
        p->free = cl->next;
        ngx_free_chain(p->pool, cl);

    } else {
        b = ngx_alloc_buf(p->pool);
        if (b == NULL) {
            return NGX_ERROR;
        }
    }

    ngx_memcpy(b, buf, sizeof(ngx_buf_t));
    b->shadow = buf;
    b->tag = p->tag;
    b->last_shadow = 1;
    b->recycled = 1;
    buf->shadow = b;

    cl = ngx_alloc_chain_link(p->pool);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    cl->buf = b;
    cl->next = NULL;

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, p->log, 0, "input buf #%d", b->num);

    if (p->in) {
        *p->last_in = cl;
    } else {
        p->in = cl;
    }
    p->last_in = &cl->next;

    if (p->length == -1) {
        return NGX_OK;
    }

    p->length -= b->last - b->pos;

    return NGX_OK;
}


static ngx_inline void
ngx_event_pipe_remove_shadow_links(ngx_buf_t *buf)
{
    ngx_buf_t  *b, *next;

    b = buf->shadow;

    if (b == NULL) {
        return;
    }

    while (!b->last_shadow) {
        next = b->shadow;

        b->temporary = 0;
        b->recycled = 0;

        b->shadow = NULL;
        b = next;
    }

    b->temporary = 0;
    b->recycled = 0;
    b->last_shadow = 0;

    b->shadow = NULL;

    buf->shadow = NULL;
}


ngx_int_t
ngx_event_pipe_add_free_buf(ngx_event_pipe_t *p, ngx_buf_t *b)
{
    ngx_chain_t  *cl;

    cl = ngx_alloc_chain_link(p->pool);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    if (p->buf_to_file && b->start == p->buf_to_file->start) {
        b->pos = p->buf_to_file->last;
        b->last = p->buf_to_file->last;

    } else {
        b->pos = b->start;
        b->last = b->start;
    }

    b->shadow = NULL;

    cl->buf = b;

    if (p->free_raw_bufs == NULL) {
        p->free_raw_bufs = cl;
        cl->next = NULL;

        return NGX_OK;
    }

    if (p->free_raw_bufs->buf->pos == p->free_raw_bufs->buf->last) {

        /* add the free buf to the list start */

        cl->next = p->free_raw_bufs;
        p->free_raw_bufs = cl;

        return NGX_OK;
    }

    /* the first free buf is partially filled, thus add the free buf after it */

    cl->next = p->free_raw_bufs->next;
    p->free_raw_bufs->next = cl;

    return NGX_OK;
}


static ngx_int_t
ngx_event_pipe_drain_chains(ngx_event_pipe_t *p)
{
    ngx_chain_t  *cl, *tl;

    for ( ;; ) {
        if (p->busy) {
            cl = p->busy;
            p->busy = NULL;

        } else if (p->out) {
            cl = p->out;
            p->out = NULL;

        } else if (p->in) {
            cl = p->in;
            p->in = NULL;

        } else {
            return NGX_OK;
        }

        while (cl) {
            if (cl->buf->last_shadow) {
                if (ngx_event_pipe_add_free_buf(p, cl->buf->shadow) != NGX_OK) {
                    return NGX_ABORT;
                }

                cl->buf->last_shadow = 0;
            }

            cl->buf->shadow = NULL;
            tl = cl->next;
            cl->next = p->free;
            p->free = cl;
            cl = tl;
        }
    }
}
