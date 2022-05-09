/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2016.  ALL RIGHTS RESERVED.
* Copyright (C) Advanced Micro Devices, Inc. 2018. ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#ifndef HAVE_CONFIG_H
#  define HAVE_CONFIG_H /* Force using config.h, so test would fail if header
                           actually tries to use it */
#endif

/*
 * UCP hello world client / server example utility
 * -----------------------------------------------
 *
 * Server side:
 *
 *    ./ucp_hello_world
 *
 * Client side:
 *
 *    ./ucp_hello_world -n <server host name>
 *
 * Notes:
 *
 *    - Client acquires Server UCX address via TCP socket
 *
 *
 * Author:
 *
 *    Ilya Nelkenbaum <ilya@nelkenbaum.com>
 *    Sergey Shalnov <sergeysh@mellanox.com> 7-June-2016
 */

#include "hello_world_util.h"

#include <ucp/api/ucp.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <assert.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  /* getopt */
#include <ctype.h>   /* isprint */
#include <pthread.h> /* pthread_self */
#include <errno.h>   /* errno */
#include <time.h>
#include <signal.h>  /* raise */
#include <arpa/inet.h>

#define IP_STRING_LEN          50
#define PORT_STRING_LEN        8


struct msg {
    uint64_t        data_len;
};

struct ucp_request_context {
    int             completed;
};

typedef struct ucp_connect_context {
    volatile ucp_conn_request_h conn_request;
    ucp_listener_h              listener;
} ucp_connect_context_t;

struct ucp_rma_context {
    uint64_t raddr;
    ucp_rkey_h rkey;
}ucp_rma_context_t;

enum ucp_example_wakeup_mode_t {
    WAKEUP_MODE_PROBE,
    WAKEUP_MODE_WAIT,
    WAKEUP_MODE_EVENTFD
} ucp_wakeup_mode = WAKEUP_MODE_PROBE;

enum ucp_example_connect_mode_t {
    CONNECT_MODE_ADDRESS,
    CONNECT_MODE_LISTENER
} ucp_connect_mode = CONNECT_MODE_ADDRESS;

enum ucp_example_communication_mode_t {
    COMMUNICATION_MODE_TAG,
    COMMUNICATION_MODE_RMA,
    COMMUNICATION_MODE_AM,
    COMMUNICATION_MODE_STREAM
} ucp_communication_mode = COMMUNICATION_MODE_TAG;


static struct err_handling {
    ucp_err_handling_mode_t ucp_err_mode;
    int                     failure;
} err_handling_opt;

static ucs_status_t client_status = UCS_OK;
static uint16_t server_port = 13337;
static long test_string_length = 16;
static const ucp_tag_t tag  = 0x1337a880u;
static const ucp_tag_t tag_mask = UINT64_MAX;
static ucp_address_t *local_addr;
static ucp_address_t *peer_addr;
static size_t local_addr_len;
static size_t peer_addr_len;
//static ucp_listener_h server_listener;
ucp_connect_context_t conn_ctx;
ucp_rma_context rma_ctx; 

void *rma_buf = NULL;
size_t rma_buf_size = 100;
static int oob_sock;

static ucs_status_t parse_cmd(int argc, char * const argv[], char **server_name, char **listen_name);

// static void set_msg_data_len(struct msg *msg, uint64_t data_len)
// {
//     mem_type_memcpy(&msg->data_len, &data_len, sizeof(data_len));
// }

static void request_init(void *request)
{
    struct ucp_request_context *ctx = (struct ucp_request_context *) request;
    ctx->completed = 0;
}

static void send_handler(void *request, ucs_status_t status, void *ctx)
{
    struct ucp_request_context *context = ctx;

    context->completed = 1;

    printf("[0x%x] send handler called with status %d (%s)\n",
           (unsigned int)pthread_self(), status, ucs_status_string(status));
}

static void failure_handler(void *arg, ucp_ep_h ep, ucs_status_t status)
{
    ucs_status_t *arg_status = (ucs_status_t *)arg;

    printf("[0x%x] failure handler called with status %d (%s)\n",
           (unsigned int)pthread_self(), status, ucs_status_string(status));

    *arg_status = status;
}

static void recv_msg_handler(void *request, ucs_status_t status,
                        ucp_tag_recv_info_t *info)
{
    struct ucp_request_context *context = (struct ucp_request_context *) request;

    context->completed = 1;

    printf("[0x%x] receive handler called with status %d (%s), length %lu\n",
           (unsigned int)pthread_self(), status, ucs_status_string(status),
           info->length);
}

static void tag_recv_handler(void *request, ucs_status_t status, const ucp_tag_recv_info_t *info, void *ctx)
{
    struct ucp_request_context *context = ctx;

    context->completed = 1;

    printf("[0x%x] receive handler called with status %d (%s), length %lu\n", (unsigned int)pthread_self(), status, ucs_status_string(status), info->length);
}

static void stream_recv_handler(void *request, ucs_status_t status, size_t length, void *ctx)
{
    struct ucp_request_context *context = ctx;

    context->completed = 1;
}

/**
 * Close the given endpoint.
 * Currently closing the endpoint with UCP_EP_CLOSE_MODE_FORCE since we currently
 * cannot rely on the client side to be present during the server's endpoint
 * closing process.
 */
static void ep_close(ucp_worker_h ucp_worker, ucp_ep_h ep)
{
    ucp_request_param_t param;
    ucs_status_t status;
    ucs_status_ptr_t close_req;

    param.op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS;
    param.flags        = UCP_EP_CLOSE_FLAG_FORCE;
    close_req = ucp_ep_close_nbx(ep, &param);
    if (UCS_PTR_IS_PTR(close_req)) {
        do {
            ucp_worker_progress(ucp_worker);
            status = ucp_request_check_status(close_req);
        } while (status == UCS_INPROGRESS);

        ucp_request_free(close_req);
    } else if (UCS_PTR_STATUS(close_req) != UCS_OK) {
        fprintf(stderr, "failed to close ep %p with status [%s]\n", (void*)ep, ucs_status_string(UCS_PTR_STATUS(close_req)));
    }
}

/**
 * The callback on the server side which is invoked upon receiving a connection
 * request from the client.
 */

static char* sockaddr_get_ip_str(const struct sockaddr_storage *sock_addr,
                                 char *ip_str, size_t max_size)
{
    struct sockaddr_in  addr_in;
    struct sockaddr_in6 addr_in6;

    switch (sock_addr->ss_family) {
    case AF_INET:
        memcpy(&addr_in, sock_addr, sizeof(struct sockaddr_in));
        inet_ntop(AF_INET, &addr_in.sin_addr, ip_str, max_size);
        return ip_str;
    case AF_INET6:
        memcpy(&addr_in6, sock_addr, sizeof(struct sockaddr_in6));
        inet_ntop(AF_INET6, &addr_in6.sin6_addr, ip_str, max_size);
        return ip_str;
    default:
        return "Invalid address family";
    }
}

static char* sockaddr_get_port_str(const struct sockaddr_storage *sock_addr,
                                   char *port_str, size_t max_size)
{
    struct sockaddr_in  addr_in;
    struct sockaddr_in6 addr_in6;

    switch (sock_addr->ss_family) {
    case AF_INET:
        memcpy(&addr_in, sock_addr, sizeof(struct sockaddr_in));
        snprintf(port_str, max_size, "%d", ntohs(addr_in.sin_port));
        return port_str;
    case AF_INET6:
        memcpy(&addr_in6, sock_addr, sizeof(struct sockaddr_in6));
        snprintf(port_str, max_size, "%d", ntohs(addr_in6.sin6_port));
        return port_str;
    default:
        return "Invalid address family";
    }
}

static void server_conn_handle_cb(ucp_conn_request_h conn_request, void *arg)
{
    ucp_connect_context_t *context = arg;
    ucp_conn_request_attr_t attr;
    char ip_str[IP_STRING_LEN];
    char port_str[PORT_STRING_LEN];
    ucs_status_t status;

    attr.field_mask = UCP_CONN_REQUEST_ATTR_FIELD_CLIENT_ADDR;
    status = ucp_conn_request_query(conn_request, &attr);
    if (status == UCS_OK) {
        printf("Server received a connection request from client at address %s:%s\n",
               sockaddr_get_ip_str(&attr.client_address, ip_str, sizeof(ip_str)),
               sockaddr_get_port_str(&attr.client_address, port_str, sizeof(port_str)));
    } else if (status != UCS_ERR_UNSUPPORTED) {
        fprintf(stderr, "failed to query the connection request (%s)\n",
                ucs_status_string(status));
    }

    if (context->conn_request == NULL) {
        context->conn_request = conn_request;
    } else {
        /* The server is already handling a connection request from a client,
         * reject this new one */
        printf("Rejecting a connection request. "
               "Only one client at a time is supported.\n");
        status = ucp_listener_reject(context->listener, conn_request);
        if (status != UCS_OK) {
            fprintf(stderr, "server failed to reject a connection request: (%s)\n",
                    ucs_status_string(status));
        }
    }
}

static void ucx_wait(ucp_worker_h ucp_worker, struct ucp_request_context *context)
{
    while (context->completed == 0) {
        ucp_worker_progress(ucp_worker);
    }
}

static ucs_status_t test_poll_wait(ucp_worker_h ucp_worker)
{
    int err            = 0;
    ucs_status_t ret   = UCS_ERR_NO_MESSAGE;
    int epoll_fd_local = 0;
    int epoll_fd       = 0;
    ucs_status_t status;
    struct epoll_event ev;
    ev.data.u64        = 0;

    status = ucp_worker_get_efd(ucp_worker, &epoll_fd);
    CHKERR_JUMP(UCS_OK != status, "ucp_worker_get_efd", err);

    /* It is recommended to copy original fd */
    epoll_fd_local = epoll_create(1);

    ev.data.fd = epoll_fd;
    ev.events = EPOLLIN;
    err = epoll_ctl(epoll_fd_local, EPOLL_CTL_ADD, epoll_fd, &ev);
    CHKERR_JUMP(err < 0, "add original socket to the new epoll\n", err_fd);

    /* Need to prepare ucp_worker before epoll_wait */
    status = ucp_worker_arm(ucp_worker);
    if (status == UCS_ERR_BUSY) { /* some events are arrived already */
        ret = UCS_OK;
        goto err_fd;
    }
    CHKERR_JUMP(status != UCS_OK, "ucp_worker_arm\n", err_fd);

    do {
        err = epoll_wait(epoll_fd_local, &ev, 1, -1);
    } while ((err == -1) && (errno == EINTR));

    ret = UCS_OK;

err_fd:
    close(epoll_fd_local);

err:
    return ret;
}

static void flush_callback(void *request, ucs_status_t status, void *user_data)
{
}

static ucs_status_t flush_ep(ucp_worker_h worker, ucp_ep_h ep)
{
    ucp_request_param_t param;
    void *request;

    param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK;
    param.cb.send      = flush_callback;
    request            = ucp_ep_flush_nbx(ep, &param);
    if (request == NULL) {
        return UCS_OK;
    } else if (UCS_PTR_IS_ERR(request)) {
        return UCS_PTR_STATUS(request);
    } else {
        ucs_status_t status;
        do {
            ucp_worker_progress(worker);
            status = ucp_request_check_status(request);
        } while (status == UCS_INPROGRESS);
        ucp_request_release(request);
        return status;
    }
}

ucs_status_t createUcpContext(ucp_context_h& ucp_context){
    ucp_params_t ucp_params;
    ucp_config_t *config;
    ucs_status_t status;

    memset(&ucp_params, 0, sizeof(ucp_params));
    status = ucp_config_read(NULL, NULL, &config);
    if(status != UCS_OK) {return status;}
    //? 这里request size相关还有必要设置吗 在有nbx的情况
    ucp_params.field_mask   = UCP_PARAM_FIELD_FEATURES |
                              UCP_PARAM_FIELD_REQUEST_SIZE |
                              UCP_PARAM_FIELD_REQUEST_INIT;
    ucp_params.features     = UCP_FEATURE_TAG;
    switch(ucp_communication_mode){
        case COMMUNICATION_MODE_RMA:ucp_params.features |= UCP_FEATURE_RMA; break;
        case COMMUNICATION_MODE_AM:ucp_params.features |= UCP_FEATURE_AM; break;
        case COMMUNICATION_MODE_STREAM:ucp_params.features |= UCP_FEATURE_STREAM; break;
        default:ucp_params.features = UCP_FEATURE_TAG;
    }
    if (ucp_wakeup_mode == WAKEUP_MODE_WAIT || ucp_wakeup_mode == WAKEUP_MODE_EVENTFD) {
        ucp_params.features |= UCP_FEATURE_WAKEUP;
    }
    ucp_params.request_size    = sizeof(struct ucp_request_context);
    ucp_params.request_init    = request_init;

    status = ucp_init(&ucp_params, config, &ucp_context);
    return status;
}

ucs_status_t registAndPackRemoteAccessMemory(ucp_context_h& ucp_context, ucp_mem_h& mem_handle, void* buf, size_t buf_size, void*& rkey_buf, size_t& rkey_buf_size){
    ucp_mem_map_params_t mem_param;
    ucs_status_t status;

    //todo 研究下分配的FLAG
    mem_param.field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS |
                           UCP_MEM_MAP_PARAM_FIELD_LENGTH;
    mem_param.address    = buf;
    mem_param.length     = buf_size;
    
    status = ucp_mem_map(ucp_context, &mem_param, &mem_handle);
    if(status!=UCS_OK) return status;

    status = ucp_rkey_pack(ucp_context, mem_handle, &rkey_buf, &rkey_buf_size);
    if(status!=UCS_OK) return status;

    return UCS_OK;
}

ucs_status_t createUcpWorker(ucp_context_h& ucp_context, ucp_worker_h& ucp_worker){
    ucp_worker_params_t worker_params;
    ucs_status_t status;

    memset(&worker_params, 0, sizeof(worker_params));
    worker_params.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_SINGLE;

    status = ucp_worker_create(ucp_context, &worker_params, &ucp_worker);
    return status;
}

ucs_status_t exchangeWorkerAddresses(ucp_worker_h& ucp_worker, const char* ip){
    ucs_status_t status;
    int ret = -1;
    uint64_t addr_len = 0;
    //| 生成worker地址
    status = ucp_worker_get_address(ucp_worker, &local_addr, &local_addr_len);
    if(status != UCS_OK){return status;}
    fprintf(stdout, "[0x%x] local address length: %lu\n",(unsigned int)pthread_self(), local_addr_len);
    
    //| 建立带外连接并交换地址
    if (ip) {
        oob_sock = client_connect(ip, server_port);
        CHKERR_JUMP(oob_sock < 0, "client_connect\n", err_addr);

        ret = recv(oob_sock, &addr_len, sizeof(addr_len), MSG_WAITALL);
        CHKERR_JUMP_RETVAL(ret != (int)sizeof(addr_len), "receive address length\n", err_addr, ret);

        peer_addr_len = addr_len;
        peer_addr = malloc(peer_addr_len);
        CHKERR_JUMP(!peer_addr, "allocate memory\n", err_addr);

        ret = recv(oob_sock, peer_addr, peer_addr_len, MSG_WAITALL);
        CHKERR_JUMP_RETVAL(ret != (int)peer_addr_len, "receive address\n", err_peer_addr, ret);
    } else {
        oob_sock = server_connect(server_port);
        CHKERR_JUMP(oob_sock < 0, "server_connect\n", err_peer_addr);

        addr_len = local_addr_len;
        ret = send(oob_sock, &addr_len, sizeof(addr_len), 0);
        CHKERR_JUMP_RETVAL(ret != (int)sizeof(addr_len), "send address length\n", err_peer_addr, ret);

        ret = send(oob_sock, local_addr, local_addr_len, 0);
        CHKERR_JUMP_RETVAL(ret != (int)local_addr_len, "send address\n", err_peer_addr, ret);
    }
    return UCS_OK;

err_peer_addr:
    free(peer_addr);

err_addr:
    ucp_worker_release_address(ucp_worker, local_addr);
    return UCS_ERR_UNSUPPORTED;
}

ucs_status_t createUcpListener(ucp_worker_h& ucp_worker, const char* ip){
    ucp_listener_params_t params;
    ucp_listener_attr_t attr;
    ucs_status_t status;
    struct sockaddr_in listen_addr;
    char ip_str[IP_STRING_LEN];
    char port_str[PORT_STRING_LEN];

    memset(&listen_addr, 0, sizeof(struct sockaddr_in));
    listen_addr.sin_family      = AF_INET;
    listen_addr.sin_addr.s_addr = (ip) ? inet_addr(ip) : INADDR_ANY;
    listen_addr.sin_port        = htons(server_port);

    params.field_mask         = UCP_LISTENER_PARAM_FIELD_SOCK_ADDR |
                                UCP_LISTENER_PARAM_FIELD_CONN_HANDLER;
    params.sockaddr.addr      = (const struct sockaddr*)&listen_addr;
    params.sockaddr.addrlen   = sizeof(listen_addr);
    params.conn_handler.cb    = server_conn_handle_cb;
    params.conn_handler.arg   = &conn_ctx;

    status = ucp_listener_create(ucp_worker, &params, &(conn_ctx.listener));
    if(status != UCS_OK) return status;
    
    attr.field_mask = UCP_LISTENER_ATTR_FIELD_SOCKADDR;
    status = ucp_listener_query(conn_ctx.listener, &attr);
    if(status != UCS_OK) {
        fprintf(stderr, "failed to query the listener (%s)\n", ucs_status_string(status));
        ucp_listener_destroy(conn_ctx.listener);
        return status;
    }
    fprintf(stderr, "server is listening on IP %s port %s\n",
            sockaddr_get_ip_str(&attr.sockaddr, ip_str, IP_STRING_LEN),
            sockaddr_get_port_str(&attr.sockaddr, port_str, PORT_STRING_LEN));
    fprintf(stdout, "Waiting for connection...\n");
    return UCS_OK;
}

ucs_status_t establishConnection(ucp_worker_h& ucp_worker, ucp_ep_h& ep, const char* ip){
    ucp_ep_params_t ep_params;
    ucp_request_param_t send_param;
    ucp_request_context ctx;
    ucs_status_ptr_t request;
    ucp_tag_message_h msg_tag;
    ucp_tag_recv_info_t info_tag;
    struct sockaddr_in connect_addr;

    void* buf = NULL;
    size_t buf_len = 0;

    ucs_status_t status;
    if(ip == NULL){
        // server
        if(ucp_connect_mode != CONNECT_MODE_LISTENER){
            // address
            fprintf(stdout, "waiting for client address...\n");
            //| 接收client的地址
            do{
                ucp_worker_progress(ucp_worker);
                msg_tag = ucp_tag_probe_nb(ucp_worker, tag, tag_mask, 1, &info_tag);
            }while(msg_tag == NULL);
            
            buf = malloc(info_tag.length);
            if(buf == NULL) return UCS_ERR_UNSUPPORTED;

            request = ucp_tag_msg_recv_nb(ucp_worker, buf, info_tag.length, ucp_dt_make_contig(1), msg_tag, recv_msg_handler);

            if (UCS_PTR_IS_ERR(request)) {
                fprintf(stderr, "unable to receive UCX address message (%s)\n",ucs_status_string(UCS_PTR_STATUS(request)));
                free(buf);
                return UCS_ERR_UNSUPPORTED;
            } else {
                /* ucp_tag_msg_recv_nb() cannot return NULL */
                assert(UCS_PTR_IS_PTR(request));
                //ucx_wait(ucp_worker, request);
                while(((struct ucp_request_context*)request)->completed == 0){
                    ucp_worker_progress(ucp_worker);
                }
                // ucx_wait(ucp_worker, &ctx);
                // ((struct ucp_request_context*)request)->completed = 0;
                ucp_request_release(request);
                printf("UCX address message was received\n");
            }

            //| 创建ep建立连接
            memcpy(&peer_addr_len, buf, sizeof(peer_addr_len));
            peer_addr = malloc(peer_addr_len);
            if(peer_addr == NULL){
                free(buf);
                return UCS_ERR_UNSUPPORTED;
            }
            memcpy(peer_addr, buf+sizeof(peer_addr_len), peer_addr_len);
            free(buf);

            ep_params.field_mask    = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                                      UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
                                      UCP_EP_PARAM_FIELD_ERR_HANDLER |
                                      UCP_EP_PARAM_FIELD_USER_DATA;
            ep_params.address       = peer_addr;
            ep_params.err_mode      = err_handling_opt.ucp_err_mode;
            ep_params.err_handler.cb= failure_handler;
            ep_params.err_handler.arg=NULL;
            ep_params.user_data     = &client_status;

            status = ucp_ep_create(ucp_worker, &ep_params, &ep);
            if(status != UCS_OK) return status;

        }else{
            // listener
            fprintf(stdout, "listening for client connecting...\n");
            //| 监听连接
            while (conn_ctx.conn_request == NULL) {
                ucp_worker_progress(ucp_worker);
            }

            ep_params.field_mask        = UCP_EP_PARAM_FIELD_ERR_HANDLER |
                                          UCP_EP_PARAM_FIELD_CONN_REQUEST;
            ep_params.conn_request      = conn_ctx.conn_request;
            ep_params.err_handler.cb    = failure_handler;
            ep_params.err_handler.arg   = NULL;

            status = ucp_ep_create(ucp_worker, &ep_params, &ep);
            if(status != UCS_OK) return status;
        }
    }else{
        // client
        if(ucp_connect_mode != CONNECT_MODE_LISTENER){
            fprintf(stdout, "sending address to server...\n");
            // address 
            //| 存储地址信息
            buf_len = local_addr_len + sizeof(local_addr_len);
            buf = malloc(buf_len);
            memset(buf, 0, buf_len);
            if(buf == NULL) {return UCS_ERR_UNSUPPORTED;}
            memcpy(buf, &local_addr_len, sizeof(local_addr_len));
            memcpy(buf+sizeof(local_addr_len), local_addr, local_addr_len);

            fprintf(stdout, "local address composed!\n");

            //| 创建ep建立连接
            ep_params.field_mask      = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                                        UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;
            ep_params.address         = peer_addr;
            ep_params.err_mode        = err_handling_opt.ucp_err_mode;

            status = ucp_ep_create(ucp_worker, &ep_params, &ep);
            if(status != UCS_OK) return status;

            //| 发送地址以供server建立连接
            ctx.completed = 0;
            send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                                      UCP_OP_ATTR_FIELD_USER_DATA;
            send_param.cb.send      = send_handler;
            send_param.user_data    = &ctx;
            request = ucp_tag_send_nbx(ep, buf, buf_len, tag, &send_param);

            fprintf(stdout, "local address sent!\n");

            if (UCS_PTR_IS_ERR(request)){
                fprintf(stderr, "unable to send UCX address message\n");
                free(buf);
                ucp_ep_destroy(ep);
            }else if(UCS_PTR_IS_PTR(request)){
                //todo 改写ucx_wait
                while(ctx.completed == 0){
                    ucp_worker_progress(ucp_worker);
                }
                // ucx_wait(ucp_worker, &ctx);
                ucp_request_release(request);
            }
        }else{
            // listener
            fprintf(stdout, "connecting to server(%s)...\n", ip);
            //| 设置连接地址
            memset(&connect_addr, 0, sizeof(struct sockaddr_in));
            connect_addr.sin_family      = AF_INET;
            connect_addr.sin_addr.s_addr = inet_addr(ip);
            connect_addr.sin_port        = htons(server_port);

            ep_params.field_mask       = UCP_EP_PARAM_FIELD_FLAGS       |
                                         UCP_EP_PARAM_FIELD_SOCK_ADDR   |
                                         UCP_EP_PARAM_FIELD_ERR_HANDLER |
                                         UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;
            ep_params.err_mode         = UCP_ERR_HANDLING_MODE_PEER;
            ep_params.err_handler.cb   = failure_handler;
            ep_params.err_handler.arg  = NULL;
            ep_params.flags            = UCP_EP_PARAMS_FLAGS_CLIENT_SERVER;
            ep_params.sockaddr.addr    = (struct sockaddr*)&connect_addr;
            ep_params.sockaddr.addrlen = sizeof(connect_addr);

            status = ucp_ep_create(ucp_worker, &ep_params, &ep);
            if(status != UCS_OK) return status;
        }
    }
    return UCS_OK;
}

ucs_status_ptr_t send_recv_tag(ucp_worker_h ucp_worker, ucp_ep_h ep, void* buf, size_t& buf_size, ucp_request_context& ctx, bool is_send = true){
    ucp_request_param_t req_param;
    ucs_status_ptr_t request;

    req_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                             UCP_OP_ATTR_FIELD_USER_DATA;
    req_param.user_data    = &ctx;

    if(is_send){
        req_param.cb.send = send_handler;
        request = ucp_tag_send_nbx(ep, buf, buf_size, tag, &req_param);
    }else{
        req_param.cb.recv = tag_recv_handler;
        request = ucp_tag_recv_nbx(ucp_worker, buf, buf_size, tag, 0, &req_param);
    }

    return request;
}

ucs_status_ptr_t send_recv_rma(ucp_worker_h ucp_worker, ucp_ep_h ep, void* buf, size_t& buf_size, ucp_request_context& ctx, bool is_send = true){
    // fprintf(stderr, "Can't support UCP-RMA mode!\n");
    // return UCS_STATUS_PTR(UCS_ERR_UNSUPPORTED);
    float* local_buf = (float*)buf;
    float* reg_buf = (float*)rma_buf;

    ucp_request_param_t req_param;
    ucs_status_ptr_t request;
    // ucp_atomic_op_t opcode = UCP_ATOMIC_OP_ADD;
    ucp_atomic_post_op_t post_opcode = UCP_ATOMIC_POST_OP_ADD;

    req_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                             UCP_OP_ATTR_FIELD_USER_DATA;
    req_param.user_data    = &ctx;
    req_param.cb.send = send_handler;
    //设置可用标志位，0表示不能用 1表示可用
    reg_buf[24] = 0;
    float flag = 0;
    if(is_send){
        //更新数据
        local_buf[1] = 10.0; 
        local_buf[8] = 20.0; 
        fprintf(stdout, "data updated!\n");
        //先确保远端数据已用，再put
        // do{
        //     // flag = ucp_get(ep, &flag, sizeof(flag), rma_ctx.raddr+(24*sizeof(float)), rma_ctx.rkey);
        // }while(flag == 0);
        while(reg_buf[24] == 0){
            sleep(1);
        }
        //像对端写入数据
        request = ucp_put_nbx(ep, buf, buf_size, rma_ctx.raddr, rma_ctx.rkey, &req_param);
        //通知对端数据已更新
        //更新数据
        // reg_buf[2] = 10.0;
        // reg_buf[7] = 20.0;
        // sleep(5);
        // flag=1;
        // ucp_put(ep, &flag, sizeof(flag), rma_ctx.raddr+(sizeof(float)*24), rma_ctx.rkey);
    }else{
        //使用当前rmabuf中数据
        sleep(5);
        fprintf(stdout, "rma_buf[1]:%f, rma_buf[8]:%f\n", reg_buf[1], reg_buf[8]);
        flag=1;
        ucp_put(ep, &flag, sizeof(flag), rma_ctx.raddr+(sizeof(float)*24), rma_ctx.rkey);
        sleep(5);
        fprintf(stdout, "rma_buf[1]:%f, rma_buf[8]:%f\n", reg_buf[1], reg_buf[8]);



        //告诉远端rma_buf可用
        //fprintf(stdout, "buf[2]:%f, buf[7]:%f\n", local_buf[2], local_buf[7]);
        //先确保对端已经更新再从远处get
        //request = ucp_get_nbx(ep, buf, buf_size, rma_ctx.raddr, rma_ctx.rkey, &req_param);
        // req_param.op_attr_mask |= UCP_OP_ATTR_FIELD_DATATYPE;
        // req_param.datatype = ucp_dt_make_contig(4);
        // ucs_status_t status;
        // status = ucp_atomic_post(ep, post_opcode, 1, 4, rma_ctx.raddr, rma_ctx.rkey);
        // request = UCS_STATUS_PTR(status);
    }

    return request;
}

ucs_status_ptr_t send_recv_am(ucp_worker_h ucp_worker, ucp_ep_h ep, void* buf, size_t& buf_size, ucp_request_context& ctx, bool is_send = true){
    fprintf(stderr, "Can't support UCP-AM mode!\n");
    return UCS_STATUS_PTR(UCS_ERR_UNSUPPORTED);
}

ucs_status_ptr_t send_recv_stream(ucp_worker_h ucp_worker, ucp_ep_h ep, void* buf, size_t& buf_size, ucp_request_context& ctx, bool is_send = true){
    //fprintf(stderr, "Can't support UCP-Stream mode!\n");
    //return UCS_STATUS_PTR(UCS_ERR_UNSUPPORTED);
    ucp_request_param_t req_param;
    ucs_status_ptr_t request;
    size_t recv_size;

    req_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                             UCP_OP_ATTR_FIELD_USER_DATA;
    req_param.user_data    = &ctx;

    if(is_send){
        req_param.cb.send = send_handler;
        request = ucp_stream_send_nbx(ep, buf, buf_size, &req_param);
    }else{
        req_param.op_attr_mask  |= UCP_OP_ATTR_FIELD_FLAGS;
        //req_param.cb.recv = recv_handler;
        req_param.cb.recv_stream = stream_recv_handler;
        request = ucp_stream_recv_nbx(ep, buf, buf_size, &recv_size, &req_param);
        if(request==NULL){
            fprintf(stdout, "Expecting receiving %d Bytes with %d actually!\n", buf_size, recv_size);
            buf_size = recv_size;
        }
    }

    return request;
}

ucs_status_t send_recv(ucp_worker_h ucp_worker, ucp_ep_h ep, void* buf, size_t& buf_size, bool is_send = true, ucp_example_communication_mode_t comm_mode = COMMUNICATION_MODE_TAG){
    ucs_status_ptr_t request = NULL;
    struct ucp_request_context ctx;
    //void (*release_request)(void*);
    //release_request = ucp_request_release;
    ctx.completed = 0;
    switch(comm_mode){
        case COMMUNICATION_MODE_STREAM:
            request = send_recv_stream(ucp_worker, ep, buf, buf_size, ctx, is_send);
            //release_request = ucp_request_free;
            break;
        case COMMUNICATION_MODE_RMA:
            request = send_recv_rma(ucp_worker, ep, buf, buf_size, ctx, is_send);
            break;
        case COMMUNICATION_MODE_AM:
            request = send_recv_am(ucp_worker, ep, buf, buf_size, ctx, is_send);
            break;
        case COMMUNICATION_MODE_TAG:
        default:request = send_recv_tag(ucp_worker, ep, buf, buf_size, ctx, is_send);
    }
    if(request == NULL){
        return UCS_OK;
    }

    //| 等待请求完成
    fprintf(stdout, "Waiting for request completed!\n");
    ucs_status_t status;
    if(UCS_PTR_IS_ERR(request)) {
        fprintf(stdout, "Request is error!\n");
        return UCS_PTR_STATUS(request);
    }
    fprintf(stdout, "Begin progress...\n");
    while(ctx.completed == 0){
        ucp_worker_progress(ucp_worker);
    }
    fprintf(stdout, "Request progressed!\n");
    status = ucp_request_check_status(request);
    //(*release_request)(request);
    ucp_request_free(request);
    fprintf(stdout, "Realse request!\n");
    return status;
}

ucs_status_t transferRemoteMemoryHandle(ucp_worker_h ucp_worker, ucp_ep_h ep, void* local_buf, void* rkey_buf, size_t rkey_buf_size){
    ucs_status_t status;

    size_t local_buf_size = sizeof(local_buf);
    void* buf;
    size_t buf_size;
    size_t buf_size_size = sizeof(buf_size);

    status = send_recv(ucp_worker, ep, &local_buf, local_buf_size);
    if(status!=UCS_OK) return status;

    status = send_recv(ucp_worker, ep, &rkey_buf_size, buf_size_size);
    if(status!=UCS_OK) return status;

    status = send_recv(ucp_worker, ep, rkey_buf, rkey_buf_size);
    if(status!=UCS_OK) return status;

    status = send_recv(ucp_worker, ep, &(rma_ctx.raddr), local_buf_size, false);
    if(status!=UCS_OK) return status;

    status = send_recv(ucp_worker, ep, &buf_size, buf_size_size, false);
    if(status!=UCS_OK) return status;
    buf = malloc(buf_size);

    status = send_recv(ucp_worker, ep, buf, buf_size, false);
    if(status!=UCS_OK) return status;

    status = ucp_ep_rkey_unpack(ep, buf, &(rma_ctx.rkey));
    if(status!=UCS_OK) return status;    

    free(buf);
    return UCS_OK;
}

ucs_status_t releaseRemoteAccessMemory(ucp_context_h ucp_context, ucp_mem_h mem_handle, void* rkey_buf){
    ucs_status_t status;
    
    ucp_rkey_destroy(rma_ctx.rkey);
    ucp_rkey_buffer_release(rkey_buf);
    status = ucp_mem_unmap(ucp_context, mem_handle);
    return status;
}


ucs_status_t communication(ucp_worker_h ucp_worker, ucp_ep_h ep, int data_counts, bool is_server){
    float* data = NULL;
    ucs_status_t status;
    size_t data_size = 0;
    data_size = sizeof(float)*data_counts;
    
    data = new float[data_counts];
    if(is_server){
        for(int i=0;i<data_counts;i++){
            data[i] = 1.1 + (i%2 == 0?i:i*(-1));
            if(ucp_communication_mode == COMMUNICATION_MODE_RMA || ucp_communication_mode == COMMUNICATION_MODE_AM){
                ((float*)rma_buf)[i] = data[i];
            }
        }
    }
    
    if(is_server){
        status = send_recv(ucp_worker, ep, data, data_size, true, ucp_communication_mode);
        //status=UCS_OK;
    }else{
        if(ucp_communication_mode == COMMUNICATION_MODE_RMA){
            for(int i=0;i<data_counts;i++){
                data[i] = 1.0;
            }
        }
        status = send_recv(ucp_worker, ep, data, data_size, false, ucp_communication_mode);
        //status=UCS_OK;
    }
    if(status != UCS_OK) {
        delete []data;
        return status;
    }

    fprintf(stdout, is_server?"[Server]\n":"[Client]\n");
    for(int i=0;i<data_counts;i++){
        fprintf(stdout, "%f\n", data[i]);
    }
    if(ucp_communication_mode == COMMUNICATION_MODE_RMA || ucp_communication_mode == COMMUNICATION_MODE_AM){
        fprintf(stdout, is_server?"[Server]\n":"[Client]\n");
        for(int i=0;i<data_counts;i++){
            fprintf(stdout, "%f\n", ((float*)rma_buf)[i]);
        }
    }
    delete []data;
    return UCS_OK;
}



int main(int argc, char **argv)
{
    ucs_status_t status;

    /* UCP handler objects */
    ucp_context_h ucp_context;
    ucp_worker_h ucp_worker;
    ucp_ep_h ep;
    ucp_mem_h memh;

    /* OOB connection vars */
    uint64_t addr_len = 0;
    char *client_target_name = NULL;
    char *server_listen_name = NULL;
    void *rkey_buf = NULL;
    size_t rkey_buf_size;
    int ret = 0;


    /* Parse the command line */
    status = parse_cmd(argc, argv, &client_target_name, &server_listen_name);
    CHKERR_JUMP(status != UCS_OK, "parse_cmd\n", err);

    status = createUcpContext(ucp_context);
    CHKERR_JUMP(status != UCS_OK, "createUcpContext\n", err);

    fprintf(stdout, "UCP context created!\n");

    if(ucp_communication_mode == COMMUNICATION_MODE_RMA || ucp_communication_mode == COMMUNICATION_MODE_AM){
        rma_buf = malloc(rma_buf_size);
        memset(rma_buf, 0, rma_buf_size);
        status = registAndPackRemoteAccessMemory(ucp_context, memh, rma_buf, rma_buf_size, rkey_buf, rkey_buf_size);
    }

    status = createUcpWorker(ucp_context, ucp_worker);
    CHKERR_JUMP(status != UCS_OK, "ucp_worker_create\n", err_cleanup);

    fprintf(stdout, "UCP worker created!\n");

    if(ucp_connect_mode != CONNECT_MODE_LISTENER){
        status = exchangeWorkerAddresses(ucp_worker, client_target_name);
        CHKERR_JUMP(status != UCS_OK, "ucp_worker_get_address\n", err_worker);
    }else if(client_target_name == NULL){
        status = createUcpListener(ucp_worker, server_listen_name);
        CHKERR_JUMP(status != UCS_OK, "ucp_worker_create_listener\n", err_worker);
    }

    fprintf(stdout, "UCP connect prepared!\n");
    
    status = establishConnection(ucp_worker, ep, client_target_name);
    CHKERR_JUMP(status != UCS_OK, "establish connection\n", err_worker);

    fprintf(stdout, "UCP connect established!\n");

    if(ucp_communication_mode == COMMUNICATION_MODE_RMA || ucp_communication_mode == COMMUNICATION_MODE_AM){
        status = transferRemoteMemoryHandle(ucp_worker, ep, rma_buf, rkey_buf, rkey_buf_size);
        CHKERR_JUMP(status != UCS_OK, "transfer remote memory handle\n", err_rma);
    }

    fprintf(stdout, "Remote access memory received!\n");

    status = communication(ucp_worker, ep, 10, client_target_name==NULL?true:false);
    CHKERR_JUMP(status != UCS_OK, "communication\n", err_ep);

    fprintf(stdout, "UCP communication finished!\n");

    if (ucp_connect_mode == CONNECT_MODE_ADDRESS){
        if(!ret && !err_handling_opt.failure) {
            /* Make sure remote is disconnected before destroying local worker */
            ret = barrier(oob_sock);
            fprintf(stdout, "OOB Barriered!\n");
        }
        close(oob_sock);
    }

    status = flush_ep(ucp_worker, ep);
    CHKERR_JUMP(status != UCS_OK, "flush ep\n", err_worker);

err_rma:
    if(ucp_communication_mode == COMMUNICATION_MODE_RMA || ucp_communication_mode == COMMUNICATION_MODE_AM){
        releaseRemoteAccessMemory(ucp_context, memh, rkey_buf);
    }
    free(rma_buf);

err_ep:
    ucp_connect_mode==CONNECT_MODE_ADDRESS?ucp_ep_destroy(ep):ep_close(ucp_worker,ep);

err_listener:
    if(ucp_connect_mode==CONNECT_MODE_LISTENER && client_target_name==NULL){
        ucp_listener_destroy(conn_ctx.listener);
        fprintf(stdout, "listener has been destoried!\n");
    } 
err_worker:
    ucp_worker_destroy(ucp_worker);

err_cleanup:
    ucp_cleanup(ucp_context);

err:
    return ret;
}

//| 输出示例参数用法
static void usage(){
    fprintf(stderr, "Usage: ucp_hello_world [parameters]\n");
    fprintf(stderr, "UCP hello world client/server example utility\n");
    fprintf(stderr, "\nParameters are:\n");
    fprintf(stderr, "  -w      Select wakeup mode to test "
            "ucp wakeup functions\n    options: w(wait) | e(eventfd) | p(probe)\n");
    fprintf(stderr, "  -c      Select connect mode to test "
            "ucp connect functions\n    options: a(address) | l(listener)\n");
    fprintf(stderr, "  -o      Select communication semantic to test "
            "ucp communication functions\n    options: t(Tag Matching) | r(RMA) | a(Active Message) | s(Stream)\n");
    fprintf(stderr, "  -e      Emulate unexpected failure on server side"
            "and handle an error on client side with enabled "
            "UCP_ERR_HANDLING_MODE_PEER\n");
    fprintf(stderr, "  -n name Set node name or IP address (only IP address when listener connect mode)"
            "of the server (required for client and should be ignored "
            "for server)\n");
    fprintf(stderr, "  -l Set IP address where server listens "
                    "(when the mode of connecting(c) is listener(l). If not specified, server uses INADDR_ANY; "
                    "Irrelevant at client)\n");
    fprintf(stderr, "  -p port Set alternative server port (default:13337)\n");
    fprintf(stderr, "  -s size Set test string length (default:16)\n");
    fprintf(stderr, "  -m <mem type>  memory type of messages\n");
    fprintf(stderr, "                 host - system memory (default)\n");
    if (check_mem_type_support(UCS_MEMORY_TYPE_CUDA)) {
        fprintf(stderr, "                 cuda - NVIDIA GPU memory\n");
    }
    if (check_mem_type_support(UCS_MEMORY_TYPE_CUDA_MANAGED)) {
        fprintf(stderr, "                 cuda-managed - NVIDIA GPU managed/unified memory\n");
    }
    fprintf(stderr, "\n");
}

static ucs_status_t parse_cmd(int argc, char * const argv[], char **server_name, char **listen_name)
{
    int c = 0, idx = 0;
    opterr = 0;

    err_handling_opt.ucp_err_mode   = UCP_ERR_HANDLING_MODE_NONE;
    err_handling_opt.failure        = 0;

    while ((c = getopt(argc, argv, "ew:c:o:n:l:p:s:m:h")) != -1) {
        switch (c) {
        case 'e':
            err_handling_opt.ucp_err_mode   = UCP_ERR_HANDLING_MODE_PEER;
            err_handling_opt.failure        = 1;
            break;
        case 'w':
            switch (int((char)*optarg)){
                case int('w'):ucp_wakeup_mode = WAKEUP_MODE_WAIT; break;
                case int('e'):ucp_wakeup_mode = WAKEUP_MODE_EVENTFD; break;
                case int('p'):ucp_wakeup_mode = WAKEUP_MODE_PROBE; break;
                default:fprintf(stderr,"Unsupport wakeup mode!\n");return UCS_ERR_UNSUPPORTED;
            }
            break;
        case 'c':
            switch (int((char)*optarg)){
                case int('a'):ucp_connect_mode = CONNECT_MODE_ADDRESS; break;
                case int('l'):ucp_connect_mode = CONNECT_MODE_LISTENER; break;
                default:fprintf(stderr,"Unsupport connect mode!\n");return UCS_ERR_UNSUPPORTED;
            }
            break;
        case 'o':
            switch (int((char)*optarg)){
                case int('t'):ucp_communication_mode = COMMUNICATION_MODE_TAG; break;
                case int('r'):ucp_communication_mode = COMMUNICATION_MODE_RMA; break;
                case int('a'):ucp_communication_mode = COMMUNICATION_MODE_AM; break;
                case int('s'):ucp_communication_mode = COMMUNICATION_MODE_STREAM; break;
                default:fprintf(stderr,"Unsupport communication mode!\n");return UCS_ERR_UNSUPPORTED;
            }
            break;
        case 'n':
            *server_name = optarg;
            break;
        case 'l':
            *listen_name = optarg;
            break;
        case 'p':
            server_port = atoi(optarg);
            if (server_port <= 0) {
                fprintf(stderr, "Wrong server port number %d\n", server_port);
                return UCS_ERR_UNSUPPORTED;
            }
            break;
        case 's':
            test_string_length = atol(optarg);
            if (test_string_length <= 0) {
                fprintf(stderr, "Wrong string size %ld\n", test_string_length);
                return UCS_ERR_UNSUPPORTED;
            }	
            break;
        case 'm':
            test_mem_type = parse_mem_type(optarg);
            if (test_mem_type == UCS_MEMORY_TYPE_LAST) {
                return UCS_ERR_UNSUPPORTED;
            }
            break;
        case '?':
            if (optopt == 's') {
                fprintf(stderr, "Option -%c requires an argument.\n", optopt);
            } else if (isprint (optopt)) {
                fprintf(stderr, "Unknown option `-%c'.\n", optopt);
            } else {
                fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
            }
            /* Fall through */
        case 'h':
        default:
            usage();
            return UCS_ERR_UNSUPPORTED;
        }
    }
    fprintf(stderr, "INFO: UCP_HELLO_WORLD server = %s port = %d\n",
            *server_name, server_port);

    for (idx = optind; idx < argc; idx++) {
        fprintf(stderr, "WARNING: Non-option argument %s\n", argv[idx]);
    }
    return UCS_OK;
}
