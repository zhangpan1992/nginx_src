
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_HTTP_CONFIG_H_INCLUDED_
#define _NGX_HTTP_CONFIG_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>



/*http配置解析参考资料: http://tech.uc.cn/?p=300*/

/*nginx配置解析的时候如果遇到"http{...}"、"server{...}"、"location...{...}"这样的一个配置块
*都会为该块创建一个ngx_http_conf_ctx_t结构
*该结构内部是3个数组

*假设配置:
 http {  
    test_str "hi"  
    ...  
    server {  
        ...  
        test_str "how"  
        location /hello {  
            ...  
        }  
        location /hi {  
            ...  
            test_str "how are u"  
        }  
        ...  
    }  
  
    server {  
        location /x {  
            ...  
        }  
        location /y {  
            ...  
        }  
    }   
  }  
  
*每个指令可出现在http{}内、server{}内、location{}内，每个指令必须由某个模块(如自定义的HTTP模块)来处理

*merge过程: 
 merge过程是按照module一个一个module的merge，
 第一步从main配置里面的servers，遍历每个server，把main里面的server配置merge到每个server的配置中，
 然后把main里面的location配置merge到每个server的location的配置中。
 第二步再次遍历每个server的locations，把这个server的location的配置merge到具体的每个location中
 
*nginx在解析遇到"http"指令时，会新分配一个ngx_http_conf_ctx_t结构，
 并且调用所有HTTP模块(注意，是以所有HTTP模块为单位的)的create_main_conf、
 create_srv_conf(创建用于存储server块相关的配置的结构体,即可能既出现在main也出现在server下的配置项，如上面的test_str)、
 create_loc_conf(创建用于存储location块相关的配置的结构体,即可能既出现在main、server也可能出现在location下的配置项，如上面的test_str)
 来初始化这个结构的main_conf/srv_conf/loc_conf/,后两者是为了后面的合并做准备的
 
*对于"server"，会新分配一个ngx_http_conf_ctx_t结构，
 并且调用所有模块的/create_srv_conf/create_loc_conf来初始化这个结构的srv_conf/loc_conf
*对于"location"，会新分配一个ngx_http_conf_ctx_t结构，
 并且调用所有模块的create_loc_conf来初始化这个结构的loc_conf

 因此上面的配置项会分配3+2+1+1 +2+1+1 = 11个结构体，但最后会合并成7个(4 location+2 server+1 main =7个)
*/
typedef struct {
    void        **main_conf;
    void        **srv_conf;
    void        **loc_conf;
} ngx_http_conf_ctx_t;


//成员皆为回调方法
typedef struct {
    ngx_int_t   (*preconfiguration)(ngx_conf_t *cf);
    ngx_int_t   (*postconfiguration)(ngx_conf_t *cf);

    void       *(*create_main_conf)(ngx_conf_t *cf);
    char       *(*init_main_conf)(ngx_conf_t *cf, void *conf);

    void       *(*create_srv_conf)(ngx_conf_t *cf);
    char       *(*merge_srv_conf)(ngx_conf_t *cf, void *prev, void *conf);

    void       *(*create_loc_conf)(ngx_conf_t *cf);
    char       *(*merge_loc_conf)(ngx_conf_t *cf, void *prev, void *conf);
} ngx_http_module_t;


#define NGX_HTTP_MODULE           0x50545448   /* "HTTP" */

#define NGX_HTTP_MAIN_CONF        0x02000000
#define NGX_HTTP_SRV_CONF         0x04000000
#define NGX_HTTP_LOC_CONF         0x08000000
#define NGX_HTTP_UPS_CONF         0x10000000
#define NGX_HTTP_SIF_CONF         0x20000000
#define NGX_HTTP_LIF_CONF         0x40000000
#define NGX_HTTP_LMT_CONF         0x80000000


#define NGX_HTTP_MAIN_CONF_OFFSET  offsetof(ngx_http_conf_ctx_t, main_conf)
#define NGX_HTTP_SRV_CONF_OFFSET   offsetof(ngx_http_conf_ctx_t, srv_conf)
#define NGX_HTTP_LOC_CONF_OFFSET   offsetof(ngx_http_conf_ctx_t, loc_conf)


#define ngx_http_get_module_main_conf(r, module)                             \
    (r)->main_conf[module.ctx_index]
#define ngx_http_get_module_srv_conf(r, module)  (r)->srv_conf[module.ctx_index]
#define ngx_http_get_module_loc_conf(r, module)  (r)->loc_conf[module.ctx_index]


#define ngx_http_conf_get_module_main_conf(cf, module)                        \
    ((ngx_http_conf_ctx_t *) cf->ctx)->main_conf[module.ctx_index]
#define ngx_http_conf_get_module_srv_conf(cf, module)                         \
    ((ngx_http_conf_ctx_t *) cf->ctx)->srv_conf[module.ctx_index]
#define ngx_http_conf_get_module_loc_conf(cf, module)                         \
    ((ngx_http_conf_ctx_t *) cf->ctx)->loc_conf[module.ctx_index]

#define ngx_http_cycle_get_module_main_conf(cycle, module)                    \
    (cycle->conf_ctx[ngx_http_module.index] ?                                 \
        ((ngx_http_conf_ctx_t *) cycle->conf_ctx[ngx_http_module.index])      \
            ->main_conf[module.ctx_index]:                                    \
        NULL)


#endif /* _NGX_HTTP_CONFIG_H_INCLUDED_ */
