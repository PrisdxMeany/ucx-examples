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
static ucp_connect_context_t conn_ctx;

static ucs_status_t parse_cmd(int argc, char * const argv[], char **server_name, char **listen_name);

static void set_msg_data_len(struct msg *msg, uint64_t data_len)
{
    mem_type_memcpy(&msg->data_len, &data_len, sizeof(data_len));
}

static void request_init(void *request)
{
    struct ucp_request_context *ctx = (struct ucp_request_context *) request;
    ctx->completed = 0;
}

static void send_handler(void *request, ucs_status_t status, void *ctx)
{
    struct ucp_request_context *context = (struct ucp_request_context *) ctx;

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

static void recv_handler(void *request, ucs_status_t status,
                        ucp_tag_recv_info_t *info)
{
    struct ucp_request_context *context = (struct ucp_request_context *) request;

    context->completed = 1;

    printf("[0x%x] receive handler called with status %d (%s), length %lu\n",
           (unsigned int)pthread_self(), status, ucs_status_string(status),
           info->length);
}

/**
 * The callback on the server side which is invoked upon receiving a connection
 * request from the client.
 */
static void server_conn_handle_cb(ucp_conn_request_h conn_request, void *arg)
{
    ucx_server_ctx_t *context = arg;
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

static int run_ucx_client(ucp_worker_h ucp_worker)
{
    ucp_request_param_t send_param;
    ucp_tag_recv_info_t info_tag;
    ucp_tag_message_h msg_tag;
    ucs_status_t status;
    ucp_ep_h server_ep;
    ucp_ep_params_t ep_params;
    struct msg *msg = 0;
    struct ucp_request_context *request;
    struct ucp_request_context ctx;
    size_t msg_len = 0;
    int ret = -1;
    char *str;

    /* Send client UCX address to server */
    ep_params.field_mask      = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                                UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;
    ep_params.address         = peer_addr;
    ep_params.err_mode        = err_handling_opt.ucp_err_mode;

    status = ucp_ep_create(ucp_worker, &ep_params, &server_ep);
    CHKERR_JUMP(status != UCS_OK, "ucp_ep_create\n", err);

    msg_len = sizeof(*msg) + local_addr_len;
    msg     = malloc(msg_len);
    CHKERR_JUMP(msg == NULL, "allocate memory\n", err_ep);
    memset(msg, 0, msg_len);

    msg->data_len = local_addr_len;
    memcpy(msg + 1, local_addr, local_addr_len);

    send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    send_param.cb.send      = send_handler;
    send_param.user_data    = &ctx;
    ctx.completed           = 0;
    request                 = ucp_tag_send_nbx(server_ep, msg, msg_len, tag,
                                               &send_param);
    if (UCS_PTR_IS_ERR(request)) {
        fprintf(stderr, "unable to send UCX address message\n");
        free(msg);
        goto err_ep;
    } else if (UCS_PTR_IS_PTR(request)) {
        ucx_wait(ucp_worker, &ctx);
        ucp_request_release(request);
    }

    free(msg);

    if (err_handling_opt.failure) {
        fprintf(stderr, "Emulating unexpected failure on client side\n");
        raise(SIGKILL);
    }

    /* Receive test string from server */
    for (;;) {

        /* Probing incoming events in non-block mode */
        msg_tag = ucp_tag_probe_nb(ucp_worker, tag, tag_mask, 1, &info_tag);
        if (msg_tag != NULL) {
            /* Message arrived */
            break;
        } else if (ucp_worker_progress(ucp_worker)) {
            /* Some events were polled; try again without going to sleep */
            continue;
        }

        /* If we got here, ucp_worker_progress() returned 0, so we can sleep.
         * Following blocked methods used to polling internal file descriptor
         * to make CPU idle and don't spin loop
         */
        if (ucp_wakeup_mode == WAKEUP_MODE_WAIT) {
            /* Polling incoming events*/
            status = ucp_worker_wait(ucp_worker);
            CHKERR_JUMP(status != UCS_OK, "ucp_worker_wait\n", err_ep);
        } else if (ucp_wakeup_mode == WAKEUP_MODE_EVENTFD) {
            status = test_poll_wait(ucp_worker);
            CHKERR_JUMP(status != UCS_OK, "test_poll_wait\n", err_ep);
        }
    }

    msg = mem_type_malloc(info_tag.length);
    CHKERR_JUMP(msg == NULL, "allocate memory\n", err_ep);

    request = ucp_tag_msg_recv_nb(ucp_worker, msg, info_tag.length,
                                  ucp_dt_make_contig(1), msg_tag,
                                  recv_handler);

    if (UCS_PTR_IS_ERR(request)) {
        fprintf(stderr, "unable to receive UCX data message (%u)\n",
                UCS_PTR_STATUS(request));
        free(msg);
        goto err_ep;
    } else {
        /* ucp_tag_msg_recv_nb() cannot return NULL */
        assert(UCS_PTR_IS_PTR(request));
        ucx_wait(ucp_worker, request);
        request->completed = 0;
        ucp_request_release(request);
        printf("UCX data message was received\n");
    }

    str = calloc(1, test_string_length);
    if (str != NULL) {
        mem_type_memcpy(str, msg + 1, test_string_length);
        printf("\n\n----- UCP TEST SUCCESS ----\n\n");
        printf("%s", str);
        printf("\n\n---------------------------\n\n");
        free(str);
    } else {
        fprintf(stderr, "Memory allocation failed\n");
        goto err_ep;
    }

    mem_type_free(msg);

    ret = 0;

err_ep:
    ucp_ep_destroy(server_ep);

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

static int run_ucx_server(ucp_worker_h ucp_worker)
{
    ucp_request_param_t send_param;
    ucp_tag_recv_info_t info_tag;
    ucp_tag_message_h msg_tag;
    ucs_status_t status;
    ucp_ep_h client_ep;
    ucp_ep_params_t ep_params;
    struct msg *msg = 0;
    struct ucp_request_context *request = 0;
    struct ucp_request_context ctx;
    size_t msg_len = 0;
    int ret;

    /* Receive client UCX address */
    do {
        /* Progressing before probe to update the state */
        ucp_worker_progress(ucp_worker);

        /* Probing incoming events in non-block mode */
        msg_tag = ucp_tag_probe_nb(ucp_worker, tag, tag_mask, 1, &info_tag);
    } while (msg_tag == NULL);

    msg = malloc(info_tag.length);
    CHKERR_ACTION(msg == NULL, "allocate memory\n", ret = -1; goto err);
    request = ucp_tag_msg_recv_nb(ucp_worker, msg, info_tag.length,
                                  ucp_dt_make_contig(1), msg_tag, recv_handler);

    if (UCS_PTR_IS_ERR(request)) {
        fprintf(stderr, "unable to receive UCX address message (%s)\n",
                ucs_status_string(UCS_PTR_STATUS(request)));
        free(msg);
        ret = -1;
        goto err;
    } else {
        /* ucp_tag_msg_recv_nb() cannot return NULL */
        assert(UCS_PTR_IS_PTR(request));
        ucx_wait(ucp_worker, request);
        request->completed = 0;
        ucp_request_release(request);
        printf("UCX address message was received\n");
    }

    peer_addr_len = msg->data_len;
    peer_addr     = malloc(peer_addr_len);
    if (peer_addr == NULL) {
        fprintf(stderr, "unable to allocate memory for peer address\n");
        free(msg);
        ret = -1;
        goto err;
    }

    memcpy(peer_addr, msg + 1, peer_addr_len);

    free(msg);

    /* Send test string to client */
    ep_params.field_mask      = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                                UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
                                UCP_EP_PARAM_FIELD_ERR_HANDLER |
                                UCP_EP_PARAM_FIELD_USER_DATA;
    ep_params.address         = peer_addr;
    ep_params.err_mode        = err_handling_opt.ucp_err_mode;
    ep_params.err_handler.cb  = failure_handler;
    ep_params.err_handler.arg = NULL;
    ep_params.user_data       = &client_status;

    status = ucp_ep_create(ucp_worker, &ep_params, &client_ep);
    CHKERR_ACTION(status != UCS_OK, "ucp_ep_create\n", ret = -1; goto err);

    msg_len = sizeof(*msg) + test_string_length;
    msg = mem_type_malloc(msg_len);
    CHKERR_ACTION(msg == NULL, "allocate memory\n", ret = -1; goto err_ep);
    mem_type_memset(msg, 0, msg_len);

    set_msg_data_len(msg, msg_len - sizeof(*msg));
    ret = generate_test_string((char *)(msg + 1), test_string_length);
    CHKERR_JUMP(ret < 0, "generate test string", err_free_mem_type_msg);

    send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    send_param.cb.send      = send_handler;
    send_param.user_data    = &ctx;
    ctx.completed           = 0;
    request                 = ucp_tag_send_nbx(client_ep, msg, msg_len, tag,
                                               &send_param);
    if (UCS_PTR_IS_ERR(request)) {
        fprintf(stderr, "unable to send UCX data message\n");
        ret = -1;
        goto err_free_mem_type_msg;
    } else if (UCS_PTR_IS_PTR(request)) {
        printf("UCX data message was scheduled for send\n");
        ucx_wait(ucp_worker, &ctx);
        ucp_request_release(request);
    }

    status = flush_ep(ucp_worker, client_ep);
    printf("flush_ep completed with status %d (%s)\n",
            status, ucs_status_string(status));

    ret = 0;

err_free_mem_type_msg:
    mem_type_free(msg);
err_ep:
    ucp_ep_destroy(client_ep);
err:
    return ret;
}

static int run_test(const char *client_target_name, ucp_worker_h ucp_worker)
{
    if (client_target_name != NULL) {
        return run_ucx_client(ucp_worker);
    } else {
        return run_ucx_server(ucp_worker);
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
    switch(ucp_communication_mode){
        case COMMUNICATION_MODE_RMA:ucp_params.features = UCP_FEATURE_RMA; break;
        case COMMUNICATION_MODE_AM:ucp_params.features = UCP_FEATURE_AM; break;
        case COMMUNICATION_MODE_STREAM:ucp_params.features = UCP_FEATURE_STREAM; break;
        case COMMUNICATION_MODE_TAG:
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
        oob_sock = client_connect(client_target_name, server_port);
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

    memset(listen_addr, 0, sizeof(struct sockaddr_in));
    listen_addr->sin_family      = AF_INET;
    listen_addr->sin_addr.s_addr = (ip) ? inet_addr(ip) : INADDR_ANY;
    listen_addr->sin_port        = htons(server_port);

    params.field_mask         = UCP_LISTENER_PARAM_FIELD_SOCK_ADDR |
                                UCP_LISTENER_PARAM_FIELD_CONN_HANDLER;
    params.sockaddr.addr      = (const struct sockaddr*)&listen_addr;
    params.sockaddr.addrlen   = sizeof(listen_addr);
    params.conn_handler.cb    = server_conn_handle_cb;
    params.conn_handler.arg   = conn_ctx;

    status = ucp_listener_create(ucp_worker, &params, &(conn_ctx.listener));
    if(status != UCS_OK) return status;
    
    attr.field_mask = UCP_LISTENER_ATTR_FIELD_SOCKADDR;
    status = ucp_listener_query(&(conn_ctx.listener), &attr);
    if(status != UCS_OK) {
        fprintf(stderr, "failed to query the listener (%s)\n", ucs_status_string(status));
        ucp_listener_destroy(&(conn_ctx.listener));
        return status;
    }
    fprintf(stderr, "server is listening on IP %s port %s\n",
            sockaddr_get_ip_str(&attr.sockaddr, ip_str, IP_STRING_LEN),
            sockaddr_get_port_str(&attr.sockaddr, port_str, PORT_STRING_LEN));
    fprintf(stdout, "Waiting for connection...\n");
    return UCS_OK;
}

ucs_status_t establishConnection(ucp_worker_h& ucp_worker, ucp_ep_h& ep,const char* ip){
    ucp_ep_params_t ep_params;
    ucp_request_param_t send_param;
    ucp_request_context ctx;
    ucs_status_ptr_t request;

    ucs_status_t status;
    if(ip == NULL){
        // server
        if(ucp_connect_mode != CONNECT_MODE_LISTENER){
            // address
            
        }else{
            // listener
        }
    }else{
        // client
        if(ucp_connect_mode != CONNECT_MODE_LISTENER){
            // address 

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
            request = ucp_tag_send_nbx(ep, )
            
        }else{
            // listener
        }
    }
}


int main(int argc, char **argv)
{
    /* UCP temporary vars */
    // ucp_params_t ucp_params;
    // ucp_worker_params_t worker_params;
    // ucp_config_t *config;
    ucs_status_t status;

    /* UCP handler objects */
    ucp_context_h ucp_context;
    ucp_worker_h ucp_worker;

    /* OOB connection vars */
    uint64_t addr_len = 0;
    char *client_target_name = NULL;
    char *server_listen_name = NULL;
    int oob_sock = -1;
    int ret = -1;

    // memset(&ucp_params, 0, sizeof(ucp_params));
    // memset(&worker_params, 0, sizeof(worker_params));

    /* Parse the command line */
    status = parse_cmd(argc, argv, &client_target_name);
    CHKERR_JUMP(status != UCS_OK, "parse_cmd\n", err);

    /* UCP initialization */
    // status = ucp_config_read(NULL, NULL, &config);
    // CHKERR_JUMP(status != UCS_OK, "ucp_config_read\n", err);

    // ucp_params.field_mask   = UCP_PARAM_FIELD_FEATURES |
    //                           UCP_PARAM_FIELD_REQUEST_SIZE |
    //                           UCP_PARAM_FIELD_REQUEST_INIT;
    // ucp_params.features     = UCP_FEATURE_TAG;
    // if (ucp_wakeup_mode == WAKEUP_MODE_WAIT || ucp_wakeup_mode == WAKEUP_MODE_EVENTFD) {
    //     ucp_params.features |= UCP_FEATURE_WAKEUP;
    // }
    // ucp_params.request_size    = sizeof(struct ucp_request_context);
    // ucp_params.request_init    = request_init;

    // status = ucp_init(&ucp_params, config, &ucp_context);

    // ucp_config_print(config, stdout, NULL, UCS_CONFIG_PRINT_CONFIG);

    // ucp_config_release(config);
    // CHKERR_JUMP(status != UCS_OK, "ucp_init\n", err);
    status = createUcpContext(ucp_context);
    CHKERR_JUMP(status != UCS_OK, "createUcpContext\n", err);

    // worker_params.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    // worker_params.thread_mode = UCS_THREAD_MODE_SINGLE;

    // status = ucp_worker_create(ucp_context, &worker_params, &ucp_worker);
    // CHKERR_JUMP(status != UCS_OK, "ucp_worker_create\n", err_cleanup);
    status = createUcpWorker(ucp_context, ucp_worker);
    CHKERR_JUMP(status != UCS_OK, "ucp_worker_create\n", err_cleanup);

    if(ucp_connect_mode != CONNECT_MODE_LISTENER){
        status = exchangeWorkerAddresses(ucp_worker, client_target_name);
        CHKERR_JUMP(status != UCS_OK, "ucp_worker_get_address\n", err_worker);
    }else if(client_target_name == NULL){
        status = createUcpListener(ucp_worker, server_listen_name);
        CHKERR_JUMP(status != UCS_OK, "ucp_worker_create_listener\n", err_worker);
    }


    
    // /* OOB connection establishment */
    // if (client_target_name) {
    //     peer_addr_len = local_addr_len;

    //     oob_sock = client_connect(client_target_name, server_port);
    //     CHKERR_JUMP(oob_sock < 0, "client_connect\n", err_addr);

    //     ret = recv(oob_sock, &addr_len, sizeof(addr_len), MSG_WAITALL);
    //     CHKERR_JUMP_RETVAL(ret != (int)sizeof(addr_len),
    //                        "receive address length\n", err_addr, ret);

    //     peer_addr_len = addr_len;
    //     peer_addr = malloc(peer_addr_len);
    //     CHKERR_JUMP(!peer_addr, "allocate memory\n", err_addr);

    //     ret = recv(oob_sock, peer_addr, peer_addr_len, MSG_WAITALL);
    //     CHKERR_JUMP_RETVAL(ret != (int)peer_addr_len,
    //                        "receive address\n", err_peer_addr, ret);
    // } else {
    //     oob_sock = server_connect(server_port);
    //     CHKERR_JUMP(oob_sock < 0, "server_connect\n", err_peer_addr);

    //     addr_len = local_addr_len;
    //     ret = send(oob_sock, &addr_len, sizeof(addr_len), 0);
    //     CHKERR_JUMP_RETVAL(ret != (int)sizeof(addr_len),
    //                        "send address length\n", err_peer_addr, ret);

    //     ret = send(oob_sock, local_addr, local_addr_len, 0);
    //     CHKERR_JUMP_RETVAL(ret != (int)local_addr_len, "send address\n",
    //                        err_peer_addr, ret);
    // }

    ret = run_test(client_target_name, ucp_worker);

    if (!ret && !err_handling_opt.failure) {
        /* Make sure remote is disconnected before destroying local worker */
        ret = barrier(oob_sock);
    }
    close(oob_sock);

// err_peer_addr:
//     free(peer_addr);

// err_addr:
//     ucp_worker_release_address(ucp_worker, local_addr);

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
    fprintf(stderr, "  -n name Set node name or IP address "
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
            switch (optarg){
                case 'w':ucp_wakeup_mode = WAKEUP_MODE_WAIT; break;
                case 'e':ucp_wakeup_mode = WAKEUP_MODE_EVENTFD; break;
                case 'p':ucp_wakeup_mode = WAKEUP_MODE_PROBE; break;
                default:fprintf(stderr,"Unsupport wakeup mode!\n");return UCS_ERR_UNSUPPORTED;
            }
            break;
        case 'c':
            switch (optarg){
                case 'a':ucp_connect_mode = CONNECT_MODE_ADDRESS; break;
                case 'l':ucp_connect_mode = CONNECT_MODE_LISTENER; break;
                default:fprintf(stderr,"Unsupport connect mode!\n");return UCS_ERR_UNSUPPORTED;
            }
            break;
        case 'o':
            switch (optarg){
                case 't':ucp_communication_mode = COMMUNICATION_MODE_TAG; break;
                case 'r':ucp_communication_mode = COMMUNICATION_MODE_RMA; break;
                case 'a':ucp_communication_mode = COMMUNICATION_MODE_AM; break;
                case 's':ucp_communication_mode = COMMUNICATION_MODE_STREAM; break;
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
