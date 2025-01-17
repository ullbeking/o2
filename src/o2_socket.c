//
//  o2_socket.c
//  O2
//
//  Created by 弛张 on 2/4/16.
//  Copyright © 2016 弛张. All rights reserved.
//
#include "ctype.h"
#include "o2_internal.h"
#include "o2_discovery.h"
#include "o2_message.h"
#include "o2_sched.h"
#include "o2_send.h"
#include "o2_interoperation.h"
#include "o2_socket.h"

#ifdef WIN32
#include <stdio.h> 
#include <stdlib.h> 
#include <windows.h>
#include <errno.h>
#include <fcntl.h>

typedef struct ifaddrs
{
    struct ifaddrs  *ifa_next;    /* Next item in list */
    char            *ifa_name;    /* Name of interface */
    unsigned int     ifa_flags;   /* Flags from SIOCGIFFLAGS */
    struct sockaddr *ifa_addr;    /* Address of interface */
    struct sockaddr *ifa_netmask; /* Netmask of interface */
    union {
        struct sockaddr *ifu_broadaddr; /* Broadcast address of interface */
        struct sockaddr *ifu_dstaddr; /* Point-to-point destination address */
    } ifa_ifu;
#define              ifa_broadaddr ifa_ifu.ifu_broadaddr
#define              ifa_dstaddr   ifa_ifu.ifu_dstaddr
    void            *ifa_data;    /* Address-specific data */
} ifaddrs;

#else
#include "sys/ioctl.h"
#include <ifaddrs.h>
#endif

static int osc_tcp_handler(SOCKET sock, process_info_ptr info);
static int read_whole_message(SOCKET sock, process_info_ptr info);
static int tcp_accept_handler(SOCKET sock, process_info_ptr info);
static void tcp_message_cleanup(process_info_ptr info);
static int tcp_recv_handler(SOCKET sock, process_info_ptr info);
static int udp_recv_handler(SOCKET sock, process_info_ptr info);

char o2_local_ip[24];
int o2_local_tcp_port = 0;
SOCKET local_send_sock = INVALID_SOCKET; // socket for sending all UDP msgs

process_info_ptr o2_message_source = NULL; ///< socket info for current message

int o2_found_network = FALSE;

static struct sockaddr_in o2_serv_addr;
int o2_socket_delete_flag = FALSE; // flag to find deleted sockets

static int bind_recv_socket(SOCKET sock, int *port, int tcp_recv_flag)
{
    memset(PTR(&o2_serv_addr), 0, sizeof(o2_serv_addr));
    o2_serv_addr.sin_family = AF_INET;
    o2_serv_addr.sin_addr.s_addr = htonl(INADDR_ANY); // local IP address
    o2_serv_addr.sin_port = htons(*port);
    unsigned int yes = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                   PTR(&yes), sizeof(yes)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        return O2_FAIL;
    }
    if (bind(sock, (struct sockaddr *) &o2_serv_addr, sizeof(o2_serv_addr))) {
        if (tcp_recv_flag) perror("Bind receive socket");
        return O2_FAIL;
    }
    if (*port == 0) { // find the port that was (possibly) allocated
        socklen_t addr_len = sizeof(o2_serv_addr);
        if (getsockname(sock, (struct sockaddr *) &o2_serv_addr,
                        &addr_len)) {
            perror("getsockname call to get port number");
            return O2_FAIL;
        }
        *port = ntohs(o2_serv_addr.sin_port);  // set actual port used
    }
    // printf("*   %s: bind socket %d port %d\n", o2_debug_prefix, sock, *port);
    assert(*port != 0);
    return O2_SUCCESS;
}


// all O2 messages arriving by TCP or UDP are funneled through here
static void deliver_or_schedule(process_info_ptr proc)
{
    // make sure endian is compatible
#if IS_LITTLE_ENDIAN
    o2_msg_swap_endian(&(proc->message->data), FALSE);
#endif

    O2_DBr(if (proc->message->data.address[1] != '_' &&
               !isdigit(proc->message->data.address[1]))
               o2_dbg_msg("msg received", &proc->message->data,
                          "type", o2_tag_to_string(proc->tag)));
    O2_DBR(if (proc->message->data.address[1] == '_' ||
               isdigit(proc->message->data.address[1]))
               o2_dbg_msg("msg received", &proc->message->data,
                          "type", o2_tag_to_string(proc->tag)));
    o2_message_source = proc;
    o2_message_send_sched(proc->message, TRUE);
}


#ifdef WIN32
static struct sockaddr *dupaddr(const sockaddr_gen * src)
{
    sockaddr_gen * d = malloc(sizeof(*d));
    if (d) {
        memcpy(d, src, sizeof(*d));
    }
    return (struct sockaddr *) d;
}


static void freeifaddrs(struct ifaddrs *ifp)
{
    struct ifaddrs *next;
    while (ifp) {
        next = ifp->ifa_next;
        free(ifp);
        ifp = next;
    }
}


static int getifaddrs(struct ifaddrs **ifpp)
{
    SOCKET sock = INVALID_SOCKET;
    size_t intarray_len = 8192;
    int ret = -1;
    INTERFACE_INFO *intarray = NULL;

    *ifpp = NULL;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET)
        return -1;

    for (;;) {
        DWORD cbret = 0;

        intarray = malloc(intarray_len);
        if (!intarray)
            break;

        ZeroMemory(intarray, intarray_len);

        if (WSAIoctl(sock, SIO_GET_INTERFACE_LIST, NULL, 0,
                     (LPVOID) intarray, (DWORD) intarray_len, &cbret,
                     NULL, NULL) == 0) {
            intarray_len = cbret;
            break;
        }

        free(intarray);
        intarray = NULL;

        if (WSAGetLastError() == WSAEFAULT && cbret > intarray_len) {
            intarray_len = cbret;
        } else {
            break;
        }
    }

    if (!intarray)
        goto _exit;

    /* intarray is an array of INTERFACE_INFO structures.  intarray_len has the
       actual size of the buffer.  The number of elements is
       intarray_len/sizeof(INTERFACE_INFO) */

    {
        size_t n = intarray_len / sizeof(INTERFACE_INFO);
        size_t i;

        for (i = 0; i < n; i++) {
            struct ifaddrs *ifp;

            ifp = malloc(sizeof(*ifp));
            if (ifp == NULL)
                break;

            ZeroMemory(ifp, sizeof(*ifp));

            ifp->ifa_next = NULL;
            ifp->ifa_name = NULL;
            ifp->ifa_flags = intarray[i].iiFlags;
            ifp->ifa_addr = dupaddr(&intarray[i].iiAddress);
            ifp->ifa_netmask = dupaddr(&intarray[i].iiNetmask);
            ifp->ifa_broadaddr = dupaddr(&intarray[i].iiBroadcastAddress);
            ifp->ifa_data = NULL;

            *ifpp = ifp;
            ifpp = &ifp->ifa_next;
        }

        if (i == n)
            ret = 0;
    }

_exit:

    if (sock != INVALID_SOCKET)
        closesocket(sock);

    if (intarray)
        free(intarray);

    return ret;
}


static int stateWSock = -1;
int o2_initWSock()
{
    WORD reqversion;
    WSADATA wsaData;
    if (stateWSock >= 0) {
        return stateWSock;
    }
    /* TODO - which version of Winsock do we actually need? */
    
    reqversion = MAKEWORD(2, 2);
    if (WSAStartup(reqversion, &wsaData) != 0) {
        /* Couldn't initialize Winsock */
        stateWSock = 0;
    } else if (LOBYTE(wsaData.wVersion) != LOBYTE(reqversion) ||
               HIBYTE(wsaData.wVersion) != HIBYTE(reqversion)) {
        /* wrong version */
        WSACleanup();
        stateWSock = 0;
    } else {
        stateWSock = 1;
    }    
    return stateWSock;
}

#endif


#ifdef SOCKET_DEBUG
void o2_sockets_show()
{
    printf("Sockets:\n");
    for (int i = 0; i < o2_context->fds.length; i++) {
        process_info_ptr proc = GET_PROCESS(i); 
        printf("%d: fd_index %d fd %lld tag %s proc %p", i, proc->fds_index,
               (long long) ((DA_GET(o2_context->fds, struct pollfd, i))->fd),
               o2_tag_to_string(proc->tag), proc);
        if (proc->tag == TCP_SOCKET) {
            printf(" services:");
            for (int i = 0; i < proc->proc.services.length; i++) {
                services_entry_ptr ss = DA_GET(proc->proc.services,
                                               proc_service_data, i)->services;
                // ugly, but just a fast test if service is _o2:
                if (*((int32_t *) (ss->key)) != *((int32_t *) "_o2")) {
                    printf("\n    %s", ss->key);
                }
            }
        } else if (proc->tag == OSC_SOCKET || 
                   proc->tag == OSC_TCP_SERVER_SOCKET ||
                   proc->tag == OSC_TCP_CLIENT) {
            printf("osc service %s", proc->osc.service_name);
        }
        printf("\n");
    }
}
#endif


process_info_ptr o2_add_new_socket(SOCKET sock, int tag,
                                   o2_socket_handler handler)
{
    // expand socket arrays for new port
    DA_EXPAND(o2_context->fds_info, process_info_ptr);
    DA_EXPAND(o2_context->fds, struct pollfd);

    process_info_ptr proc = (process_info_ptr)
            O2_CALLOC(1, sizeof(process_info));
    *DA_LAST(o2_context->fds_info, process_info_ptr) = proc;
    proc->tag = tag;
    proc->fds_index = o2_context->fds.length - 1; // last element
    proc->handler = handler;
    proc->delete_me = FALSE;

    struct pollfd *pfd = DA_LAST(o2_context->fds, struct pollfd);
    pfd->fd = sock;
    pfd->events = POLLIN;
    pfd->revents = 0;
    return proc;
}


int o2_process_initialize(process_info_ptr proc, int status, int hub_flag)
{
    proc->proc.status = status;
    proc->proc.uses_hub = hub_flag;
    proc->port = 0;
    memset(&proc->proc.udp_sa, 0, sizeof(proc->proc.udp_sa));
    proc->proc.pending_msg = NULL;
    return O2_SUCCESS;
}


/**
 *  Initialize discovery, tcp, and udp sockets.
 *
 *  @return 0 (O2_SUCCESS) on success, -1 (O2_FAIL) on failure.
 */
int o2_sockets_initialize()
{
#ifdef WIN32
    // Initialize (in Windows)
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#else
    DA_INIT(o2_context->fds, struct pollfd, 5);
#endif // WIN32
    
    DA_INIT(o2_context->fds_info, process_info_ptr, 5);
    memset(o2_context->fds_info.array, 0, 5 * sizeof(process_info_ptr));
    
    // Set a broadcast socket. If cannot set up,
    //   print the error and return O2_FAIL
    RETURN_IF_ERROR(o2_discovery_initialize());

    // make udp receive socket for incoming O2 messages
    int port = 0;
    process_info_ptr proc;
    RETURN_IF_ERROR(o2_make_udp_recv_socket(UDP_SOCKET, &port, &proc));
    // ignore the proc for udp, get the proc for tcp:
    // Set up the tcp server socket.
    RETURN_IF_ERROR(o2_make_tcp_recv_socket(TCP_SERVER_SOCKET, 0,
                                            &tcp_accept_handler, &o2_context->process));
    assert(port != 0);
    o2_context->process->port = port;
    
    // more initialization in discovery, depends on tcp port which is now set
    RETURN_IF_ERROR(o2_discovery_msg_initialize());

/* IF THAT FAILS, HERE'S OLDER CODE TO GET THE TCP PORT
    struct sockaddr_in sa;
    socklen_t restrict sa_len = sizeof(sa);
    if (getsockname(o2_context->process.tcp_socket, (struct sockaddr *restrict)
                    &sa, &sa_len) < 0) {
        perror("Getting port number from tcp server socket");
        return O2_FAIL;
    }
    o2_context->process.tcp_port = ntohs(sa.sin_port);
*/
    return O2_SUCCESS;
}


// Add a socket for TCP to sockets arrays o2_context->fds and
// o2_context->fds_info.  As a side effect, if this is the TCP server
// socket, the o2_context->process.key will be set to the server IP address
int o2_make_tcp_recv_socket(int tag, int port,
                            o2_socket_handler handler, process_info_ptr *proc)
{
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    char name[32]; // "100.100.100.100:65000" -> 21 chars
    struct ifaddrs *ifap, *ifa;
    name[0] = 0;   // initially empty
    if (sock == INVALID_SOCKET) {
        printf("tcp socket set up error");
        return O2_FAIL;
    }
    O2_DBo(printf("%s created tcp socket %ld tag %s\n",
                  o2_debug_prefix, (long) sock, o2_tag_to_string(tag)));
    if (tag == TCP_SERVER_SOCKET || tag == OSC_TCP_SERVER_SOCKET) {
        // only bind server port
        RETURN_IF_ERROR(bind_recv_socket(sock, &port, TRUE));
        RETURN_IF_ERROR(listen(sock, 10));
        O2_DBo(printf("%s bind and listen called on socket %ld\n",
                      o2_debug_prefix, (long) sock));
    }
    *proc = o2_add_new_socket(sock, tag, handler);
    if (tag == TCP_SERVER_SOCKET) {
        o2_local_tcp_port = port;
        struct sockaddr_in *sa;
        // look for AF_INET interface. If you find one, copy it
        // to name. If you find one that is not 127.0.0.1, then
        // stop looking.
        
        if (getifaddrs(&ifap)) {
            perror("getting IP address");
            return O2_FAIL;
        }
        for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr->sa_family==AF_INET) {
                sa = (struct sockaddr_in *) ifa->ifa_addr;
                if (!inet_ntop(AF_INET, &sa->sin_addr, o2_local_ip,
                               sizeof(o2_local_ip))) {
                    perror("converting local ip to string");
                    break;
                }
                sprintf(name, "%s:%d", o2_local_ip, port);
                if (!streql(o2_local_ip, "127.0.0.1")) {
                    o2_found_network = O2_TRUE;
                    break;
                }
            }
        }
        freeifaddrs(ifap);
        (*proc)->proc.name = o2_heapify(name);
        RETURN_IF_ERROR(o2_process_initialize(*proc, PROCESS_LOCAL, O2_NO_HUB));
    } else { // a "normal" TCP connection: set NODELAY option
        // (NODELAY means that TCP messages will be delivered immediately
        // rather than waiting a short period for additional data to be
        // sent. Waiting might allow the outgoing packet to consolidate 
        // sent data, resulting in greater throughput, but more latency.
        int option = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *) &option,
                   sizeof(option));
        if (tag == OSC_TCP_SERVER_SOCKET) {
            (*proc)->port = port;
        }
    }
    return O2_SUCCESS;
}

                                    
int o2_make_udp_recv_socket(int tag, int *port, process_info_ptr *proc)
{
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET)
        return O2_FAIL;
    // Bind the socket
    int err;
    if ((err = bind_recv_socket(sock, port, FALSE))) {
        closesocket(sock);
        return err;
    }
    O2_DBo(printf("%s created socket %ld and bind called to receive UDP\n",
                  o2_debug_prefix, (long) sock));
    *proc = o2_add_new_socket(sock, tag, &udp_recv_handler);
    (*proc)->port = *port;
    // printf("%s: o2_make_udp_recv_socket: listening on port %d\n", o2_debug_prefix, o2_context->process.port);
    return O2_SUCCESS;
}


// When service is delegated to OSC via TCP, a TCP connection
// is created. Incoming messages are delivered to this
// handler.
//
int o2_osc_delegate_handler(SOCKET sock, process_info_ptr proc)
{
    int n = read_whole_message(sock, proc);
    if (n == O2_FAIL) { // not ready to process message yet
        return O2_SUCCESS;
    } else if (n != O2_SUCCESS) {
        return n;
    }
    O2_DBg(printf("%s ### ERROR: unexpected message from OSC server providing service %s\n", o2_debug_prefix, proc->osc.service_name));
    tcp_message_cleanup(proc);
    return O2_SUCCESS;
}


// remove the i'th socket from o2_context->fds and o2_context->fds_info
//
void o2_socket_remove(int i)
{
    struct pollfd *pfd = DA_GET(o2_context->fds, struct pollfd, i);

    O2_DBo(printf("%s o2_socket_remove(%d), tag %d port %d closing socket %lld\n",
                  o2_debug_prefix, i,
                  GET_PROCESS(i)->tag,
                  GET_PROCESS(i)->port,
                  (long long) pfd->fd));
    SOCKET sock = pfd->fd;
#ifdef SHUT_WR
    shutdown(sock, SHUT_WR);
#endif
    O2_DBo(printf("calling closesocket(%lld).\n", (int64_t) (pfd->fd)));
    if (closesocket(pfd->fd)) perror("closing socket");
    if (o2_context->fds.length > i + 1) { // move last to i
        struct pollfd *lastfd = DA_LAST(o2_context->fds, struct pollfd);
        memcpy(pfd, lastfd, sizeof(struct pollfd));
        process_info_ptr proc = *DA_LAST(o2_context->fds_info, process_info_ptr);
        GET_PROCESS(i) = proc; // move to new index
        proc->fds_index = i;
    }
    o2_context->fds.length--;
    o2_context->fds_info.length--;
}


void o2_free_deleted_sockets()
{
    for (int i = 0; i < o2_context->fds_info.length; i++) {
        process_info_ptr proc = GET_PROCESS(i);
        if (proc->delete_me) {
            o2_socket_remove(i);
            O2_FREE(proc);
            i--;
        }
    }
    o2_socket_delete_flag = FALSE;
}


#ifdef WIN32

FD_SET o2_read_set;
FD_SET o2_write_set;
struct timeval o2_no_timeout;

int o2_recv()
{
    // if there are any bad socket descriptions, remove them now
    if (o2_socket_delete_flag) o2_free_deleted_sockets();
    
    int total;
    
    FD_ZERO(&o2_read_set);
    FD_ZERO(&o2_write_set);
    for (int i = 0; i < o2_context->fds.length; i++) {
        struct pollfd *d = DA_GET(o2_context->fds, struct pollfd, i);
        FD_SET(d->fd, &o2_read_set);
        process_ifo_ptr proc = GET_PROCESS(i);
        if (proc->tag == TCP_SOCKET && proc->proc.pending_msg) {
            FD_SET(d->fd, &o2_write_set);
        }
    }
    o2_no_timeout.tv_sec = 0;
    o2_no_timeout.tv_usec = 0;
    if ((total = select(0, &o2_read_set, &o2_write_set, NULL, 
                        &o2_no_timeout)) == SOCKET_ERROR) {
        /* TODO: error handling here */
        return O2_FAIL; /* TODO: return a specific error code for this */
    }
    if (total == 0) { /* no messages waiting */
        return O2_SUCCESS;
    }
    for (int i = 0; i < o2_context->fds.length; i++) {
        struct pollfd *pfd = DA_GET(o2_context->fds, struct pollfd, i);
        if (FD_ISSET(pfd->fd, &o2_read_set)) {
            process_info_ptr proc = GET_PROCESS(i);
            if (((*(proc->handler))(pfd->fd, proc)) == O2_TCP_HUP) {
                O2_DBo(printf("%s removing remote process after O2_TCP_HUP to "
                              "socket %ld", o2_debug_prefix, (long) pfd->fd));
                o2_remove_remote_process(proc);
            }
        }
        if (FD_ISSET(pfd->fd, &o2_write_set)) {
            process_info_ptr proc = GET_PROCESS(i);
            o2_message_ptr msg = proc->proc.pending_msg; // unlink the pending msg
            proc->proc.pending_msg = NULL;
            int rslt = o2_send_message(pfd->fd, msg, FALSE, proc);
            if (rslt == O2_SUCCESS) {
                pfd->events &= ~POLLOUT;
            }
        }            
        if (!o2_ensemble_name) { // handler called o2_finish()
            // o2_context->fds are all freed and gone
            return O2_FAIL;
        }
    }
    // clean up any dead sockets before user has a chance to do anything
    // (actually, user handlers could have done a lot, so maybe this is
    // not strictly necessary.)
    if (o2_socket_delete_flag) o2_free_deleted_sockets();
    return O2_SUCCESS;
}

#else  // Use poll function to receive messages.

int o2_recv()
{
    int i;
        
    // if there are any bad socket descriptions, remove them now
    if (o2_socket_delete_flag) o2_free_deleted_sockets();

    poll((struct pollfd *) o2_context->fds.array, o2_context->fds.length, 0);
    int len = o2_context->fds.length; // length can grow while we're looping!
    for (i = 0; i < len; i++) {
        process_info_ptr proc;
        struct pollfd *pfd = DA_GET(o2_context->fds, struct pollfd, i);
        // if (d->revents) printf("%d:%p:%x ", i, d, d->revents);
        if (pfd->revents & POLLERR) {
        } else if (pfd->revents & POLLHUP) {
            proc = GET_PROCESS(i);
            O2_DBo(printf("%s removing remote process after POLLHUP to "
                          "socket %ld\n", o2_debug_prefix, (long) (pfd->fd)));
            o2_remove_remote_process(proc);
        } else if (pfd->revents & POLLIN) {
            proc = GET_PROCESS(i);
            assert(proc->length_got < 5);
            if ((*(proc->handler))(pfd->fd, proc)) {
                O2_DBo(printf("%s removing remote process after handler "
                              "reported error on socket %ld", o2_debug_prefix, 
                              (long) (pfd->fd)));
                o2_remove_remote_process(proc);
            }
        } else if (pfd->revents & POLLOUT) {
            proc = GET_PROCESS(i); // find the process info
            o2_message_ptr msg = proc->proc.pending_msg; // unlink the pending msg
            proc->proc.pending_msg = NULL;
            int rslt = o2_send_message(pfd, msg, FALSE, proc);
            if (rslt == O2_SUCCESS) {
                pfd->events &= ~POLLOUT;
            }
        }
        if (!o2_ensemble_name) { // handler called o2_finish()
            // o2_context->fds are all free and gone now
            return O2_FAIL;
        }
    }
    // clean up any dead sockets before user has a chance to do anything
    // (actually, user handlers could have done a lot, so maybe this is
    // not strictly necessary.)
    if (o2_socket_delete_flag) o2_free_deleted_sockets();
    return O2_SUCCESS;
}
#endif


void o2_socket_mark_to_free(process_info_ptr proc)
{
    proc->delete_me = TRUE;
    o2_socket_delete_flag = TRUE;
}


// When a connection is accepted, we cannot know the name of the
// connecting process, so the first message that arrives will have
// to be to /_o2/in, type="ssii", ensemble name, ip, udp, tcp
// We then create a process (if not discovered yet) and associate
// this socket with the process
//
int o2_tcp_initial_handler(SOCKET sock, process_info_ptr proc)
{
    int n = read_whole_message(sock, proc);
    if (n == O2_FAIL) { // not ready to process message yet
        return O2_SUCCESS;
    } else if (n != O2_SUCCESS) {
        return n;
    }

    // message should be addressed to !_o2/in
    char *ptr = proc->message->data.address;
    if (strcmp(ptr, "!_o2/in") != 0) { // error: close the socket
        // special case: this could be a !_o2/dy message generated by
        // a call to o2_hub(). NOTE: I don't like the idea that we
        // install a special tcp_initial_handler because we're always
        // expecting an !_o2/in message, but then we hack in an
        // exception to support o2_hub(). And note that there are 2
        // different policies on endian fixup:
        // Inside deliver_of_schedule() for !_o2/dy, but we swap
        // endian in the else clause if this is !_o2/in.
        // There must be a cleaner way.
        if (strcmp(ptr, "!_o2/dy") != 0) {
            return O2_FAIL;
        }
        // endian fixup is included in this handler:
        deliver_or_schedule(proc);
        // proc->message is now freed
    } else {
#if IS_LITTLE_ENDIAN
        o2_msg_swap_endian(&proc->message->data, FALSE);
#endif
        // types will be after "!_o2/in<0>,"
        ptr += 9; // skip over the ',' too
        o2_discovery_init_handler(&proc->message->data, ptr, NULL, 0, proc);
        proc->handler = &tcp_recv_handler;
        // since we called o2_discovery_init_handler directly,
        //   we need to free the message
        o2_message_free(proc->message);
    }
    tcp_message_cleanup(proc);
    return O2_SUCCESS;
}


// On OS X, need to disable SIGPIPE when socket is created
void o2_disable_sigpipe(SOCKET sock)
{
#ifdef __APPLE__
    int set = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE,
                   (void *) &set, sizeof(int)) < 0) {
        perror("in setsockopt in o2_disable_sigpipe");
    }
#endif    
}

// This handler is for an OSC tcp server listen socket. When it is
// "readable" this handler is called to accept the connection
// request, creating a OSC_TCP_SOCKET
//
int o2_osc_tcp_accept_handler(SOCKET sock, process_info_ptr proc)
{
    // note that this handler does not call read_whole_message()
    // printf("%s: accepting a tcp connection\n", o2_debug_prefix);
    assert(proc->tag == OSC_TCP_SERVER_SOCKET);
    SOCKET connection = accept(sock, NULL, NULL);
    if (connection == INVALID_SOCKET) {
        O2_DBg(printf("%s o2_osc_tcp_accept_handler failed to accept\n", o2_debug_prefix));
        return O2_FAIL;
    }
    o2_disable_sigpipe(connection);
    process_info_ptr conn_info = o2_add_new_socket(connection, OSC_TCP_SOCKET, &osc_tcp_handler);
    assert(proc->osc.service_name);
    conn_info->osc.service_name = proc->osc.service_name;
    assert(proc->port != 0);
    conn_info->port = proc->port;
    O2_DBoO(printf("%s OSC server on port %d accepts client as socket %ld for service %s\n",
                  o2_debug_prefix, proc->port, (long) connection, proc->osc.service_name));
    return O2_SUCCESS;
}


// socket handler to forward incoming OSC to O2 service
//
static int osc_tcp_handler(SOCKET sock, process_info_ptr proc)
{
    int n = read_whole_message(sock, proc);
    if (n == O2_FAIL) { // not ready to process message yet
        return O2_SUCCESS;
    } else if (n != O2_SUCCESS) {
        return n;
    }
    /* got the message, deliver it */
    // endian corrections are done in handler
    RETURN_IF_ERROR(o2_deliver_osc(proc));
    // proc->message is now freed
    tcp_message_cleanup(proc);
    return O2_SUCCESS;
}
        
        
// returns O2_SUCCESS if whole message is read.
//         O2_FAIL if whole message is not read yet.
//         O2_TCP_HUP if socket is closed
//
static int read_whole_message(SOCKET sock, process_info_ptr proc)
{
    assert(proc->length_got < 5);
    // printf("--   %s: read_whole message length_got %d length %d message_got %d\n",
    //       o2_debug_prefix, proc->length_got, proc->length, proc->message_got);
    /* first read length if it has not been read yet */
    if (proc->length_got < 4) {
        // coerce to int to avoid compiler warning; requested length is
        // int, so int is ok for n
        int n = (int) recvfrom(sock, PTR(&(proc->length)) + proc->length_got,
                               4 - proc->length_got, 0, NULL, NULL);
        if (n == 0) { /* socket was gracefully closed */
            O2_DBo(printf("recvfrom returned 0: deleting socket\n"));
            tcp_message_cleanup(proc);
            return O2_TCP_HUP;
        } else if (n < 0) { /* error: close the socket */
#ifdef WIN32
            if (WSAGetLastError() != WSAEWOULDBLOCK &&
                WSAGetLastError() != WSAEINTR) {
#else
            if (errno != EAGAIN && errno != EINTR) {
#endif
                perror("recvfrom in read_whole_message getting length");
                tcp_message_cleanup(proc);
                return O2_TCP_HUP;
            }
        }
        proc->length_got += n;
        assert(proc->length_got < 5);
        if (proc->length_got < 4) {
            return O2_FAIL;
        }
        // done receiving length bytes
        proc->length = htonl(proc->length);
        proc->message = o2_alloc_size_message(proc->length);
        proc->message_got = 0; // just to make sure
    }

    /* read the full message */
    if (proc->message_got < proc->length) {
        // coerce to int to avoid compiler warning; message length is int, so n can be int
        int n = (int) recvfrom(sock,
                               PTR(&(proc->message->data)) + proc->message_got,
                               proc->length - proc->message_got, 0, NULL, NULL);
        if (n == 0) { /* socket was gracefully closed */
            O2_DBo(printf("recvfrom returned 0: deleting socket\n"));
            tcp_message_cleanup(proc);
            return O2_TCP_HUP;
        } else if (n <= 0) {
#ifdef WIN32
            if (WSAGetLastError() != WSAEWOULDBLOCK &&
                WSAGetLastError() != WSAEINTR) {
#else
            if (errno != EAGAIN && errno != EINTR) {
#endif
                perror("recvfrom in read_whole_message getting data");
                o2_message_free(proc->message);
                tcp_message_cleanup(proc);
                return O2_TCP_HUP;
            }
        }
        proc->message_got += n;
        if (proc->message_got < proc->length) {
            return O2_FAIL; 
        }
    }
    proc->message->length = proc->length;
    return O2_SUCCESS; // we have a full message now
}


// This handler is for the tcp server listen socket. When it is
// "readable" this handler is called to accept the connection
// request.
//
static int tcp_accept_handler(SOCKET sock, process_info_ptr proc)
{
    // note that this handler does not call read_whole_message()
    SOCKET connection = accept(sock, NULL, NULL);
    if (connection == INVALID_SOCKET) {
        O2_DBg(printf("%s tcp_accept_handler failed to accept\n", o2_debug_prefix));
        return O2_FAIL;
    }
    int set = 1;
#ifdef __APPLE__
    setsockopt(connection, SOL_SOCKET, SO_NOSIGPIPE,
               (void *) &set, sizeof(int));
#endif
    process_info_ptr conn_info = o2_add_new_socket(connection, TCP_SOCKET, &o2_tcp_initial_handler);
    conn_info->proc.status = PROCESS_CONNECTED;
    O2_DBdo(printf("%s O2 server socket %ld accepts client as socket %ld\n",
                   o2_debug_prefix, (long) sock, (long) connection));
    return O2_SUCCESS;
}

        
static void tcp_message_cleanup(process_info_ptr proc)
{
    /* clean up for next message */
    proc->message = NULL;
    proc->message_got = 0;
    proc->length = 0;
    proc->length_got = 0;
}    


static int tcp_recv_handler(SOCKET sock, process_info_ptr proc)
{
    int n = read_whole_message(sock, proc);
    if (n == O2_FAIL) { // not ready to process message yet
        return O2_SUCCESS;
    } else if (n != O2_SUCCESS) {
        return n;
    }
    // endian fixup is included in this handler:
    deliver_or_schedule(proc);
    // proc->message is now freed
    tcp_message_cleanup(proc);
    return O2_SUCCESS;
}


static int udp_recv_handler(SOCKET sock, process_info_ptr proc)
{
    int len;
    if (ioctlsocket(sock, FIONREAD, &len) == -1) {
        perror("udp_recv_handler");
        return O2_FAIL;
    }
    proc->message = o2_alloc_size_message(len);
    if (!proc->message) return O2_FAIL;
    int n;
    // coerce to int to avoid compiler warning; len is int, so int is good for n
    if ((n = (int) recvfrom(sock, (char *) &(proc->message->data), len, 
                            0, NULL, NULL)) <= 0) {
        // I think udp errors should be ignored. UDP is not reliable
        // anyway. For now, though, let's at least print errors.
        perror("recvfrom in udp_recv_handler");
        o2_message_free(proc->message);
        proc->message = NULL;
        return O2_FAIL;
    }
    proc->message->length = n;
    // endian corrections are done in handler
    if (proc->tag == UDP_SOCKET || proc->tag == DISCOVER_SOCKET) {
        deliver_or_schedule(proc);
    } else if (proc->tag == OSC_SOCKET) {
        return o2_deliver_osc(proc);
    } else {
        assert(FALSE); // unexpected tag in fd_info
        return O2_FAIL;
    }
    proc->message = NULL; // message is deleted by now
    return O2_SUCCESS;
}


