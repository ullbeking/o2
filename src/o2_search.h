//
//  o2_search.h
//  o2
//
//  Created by 弛张 on 3/14/16.
//
//

#ifndef o2_search_h
#define o2_search_h

#ifdef __cplusplus
extern "C" {
#endif

#define PATTERN_NODE 10
#define PATTERN_HANDLER 11
#define SERVICES 12
#define O2_BRIDGE_SERVICE 13
#define OSC_REMOTE_SERVICE 14
#define TAP 15

/**
 *  Structures for hash look up.
 */

// The structure of an entry in hash table. Subclasses o2_info
// subclasses are node_entry, handler_entry, and services_entry
typedef struct o2_entry { // "subclass" of o2_info
    int tag;
    o2string key; // key is "owned" by this generic entry struct
    struct o2_entry *next;
} o2_entry, *o2_entry_ptr;


// Hash table's entry for node, another hash table
typedef struct node_entry { // "subclass" of o2_entry
    int tag; // must be PATTERN_NODE
    o2string key; // key is "owned" by this node_entry struct
    o2_entry_ptr next;
    int num_children;
    dyn_array children; // children is a dynamic array of o2_entry_ptr.
    // At the top level, all children are services_entry_ptrs (tag
    // SERVICES). Below that level children can have tags PATTERN_NODE
    // or PATTERN_HANDLER.
} node_entry, *node_entry_ptr;


// Hash table's entry for handler
typedef struct handler_entry { // "subclass" of o2_entry
    int tag; // must be PATTERN_HANDLER
    o2string key; // key is "owned" by this handler_entry struct
    o2_entry_ptr next;
    o2_method_handler handler;
    void *user_data;
    char *full_path; // this is the key for this entry in the o2_context->full_path_table
    // it is a copy of the key in the master table entry, so you should never
    // free this pointer -- it will be freed when the master table entry is
    // freed.
    o2string type_string; ///< types expected by handler, or NULL to ignore
    int types_len;     ///< the length of type_string
    int coerce_flag;   ///< boolean - coerce types to match type_string?
                       ///<   The message is not altered, but args will point
                       ///<   to copies of type-coerced data as needed
                       ///<   (coerce_flag is only set if parse_args is true.)
    int parse_args;    ///< boolean - send argc and argv to handler?
} handler_entry, *handler_entry_ptr;


typedef struct services_entry { // "subclass" of o2_entry
    int tag; // must be SERVICES
    o2string key; // key (service name) is "owned" by this struct
    o2_entry_ptr next;
    dyn_array services; // links to offers of this service. First in list
            // is the service to send to. Here "offers" means a node_entry
            // (local service), handler_entry (local service with just one
            // handler for all messages), process_info (for remote
            // service), osc_info (for service delegated to OSC server), or
            // bridge_info (for a bridge over alternate non-IP transport).
            // Valid tags for services in this array are:
            //    PATTERN_NODE, PATTERN_HANDLER, O2_BRIDGE_SERVICE,
            //    OSC_REMOTE_SERVICE, TAP, TCP_SOCKET
    dyn_array taps; // the "taps" on this service -- these are of type
            // tap_info_ptr and indicate services that should get copies
            // of messages sent to the service named by key. 
} services_entry, *services_entry_ptr;


typedef struct tap_info { // "subclass" of o2_info
    int tag; // must be TAP
    o2string tapper;
    process_info_ptr proc;
} tap_info, *tap_info_ptr;


/*
// Hash table's entry for a remote service
typedef struct remote_service_info {
    int tag;   // must be O2_REMOTE_SERVICE
    // char *key; // key is "owned" by this remote_service_entry struct
    // o2_entry_ptr next;
    process_info_ptr process;   // points to its host process for the service,
    // the remote service might be discovered but not connected
} remote_service_info, *remote_service_info_ptr;
*/

// Hash table entry for o2_osc_delegate: this service
//    is provided by an OSC server
typedef struct osc_info {
    int tag; // must be OSC_REMOTE_SERVICE
    o2string service_name;
    struct sockaddr_in udp_sa; // address for sending UDP messages
    int port;
    process_info_ptr tcp_socket_info; // points to "process" that has
    // info about the TCP socket to the OSC server, if NULL, then use UDP
} osc_info, *osc_info_ptr;


// To enumerate elements of a hash table (at one level), use this
// structure and see o2_enumerate_begin(), o2_enumerate_next()
typedef struct enumerate {
    dyn_array_ptr dict;
    int index;
    o2_entry_ptr entry;
} enumerate, *enumerate_ptr;


void o2_enumerate_begin(enumerate_ptr enumerator, dyn_array_ptr dict);


o2_entry_ptr o2_enumerate_next(enumerate_ptr enumerator);


typedef struct {
    o2_message_ptr message_freelist;
    // msg_types is used to hold type codes as message args are accumulated
    dyn_array msg_types;
    // msg_data is used to hold data as message args are accumulated
    dyn_array msg_data;
    o2_arg_ptr *argv; // arg vector extracted by calls to o2_get_next()

    int argc; // length of argv

    // o2_argv_data is used to create the argv for handlers. It is expanded as
    // needed to handle the largest message and is reused.
    dyn_array argv_data;

    // o2_arg_data holds parameters that are coerced from message data
    // It is referenced by o2_argv_data and expanded as needed.
    dyn_array arg_data;

    node_entry full_path_table;
    node_entry path_tree;
        
    process_info_ptr process; ///< the process descriptor for this process
    int using_a_hub; // set to true if o2_hub() is called;
                     // turns off broadcasting
    dyn_array fds;      ///< pre-constructed fds parameter for poll()
    dyn_array fds_info; ///< info about sockets

} o2_context_t, *o2_context_ptr;

/* O2 should not be called from multiple threads. One exception
 * is the o2x_ functions are designed to run in a high-priority thread
 * (such as an audio callback) that exchanges messages with a full O2
 * process. There is a small problem that O2 message construction and
 * decoding functions use some static, preallocated storage, so sharing
 * across threads is not allowed. To avoid this, we put shared storage
 * in an o2_context_t structure. One structure must be allocated per
 * thread, and we use a thread-local variable o2_thread_info to locate
 * the context.
 */
extern thread_local o2_context_ptr o2_context;


#ifndef O2_NO_DEBUGGING
const char *o2_tag_to_string(int tag);
#define SEARCH_DEBUG
#ifdef SEARCH_DEBUG
void o2_info_show(o2_info_ptr info, int indent);
#endif
#endif

    
services_entry_ptr o2_must_get_services(o2string service_name);

void o2_string_pad(char *dst, const char *src);

int o2_add_entry_at(node_entry_ptr node, o2_entry_ptr *loc,
                    o2_entry_ptr entry);

/**
 * add an entry to a hash table
 */
int o2_entry_add(node_entry_ptr node, o2_entry_ptr entry);


/**
 * make a new node
 */
node_entry_ptr o2_node_new(const char *key);

int o2_service_provider_replace(process_info_ptr proc, const char *service_name,
                                o2_info_ptr new_service);

int o2_service_remove(const char *service_name, process_info_ptr proc,
                      services_entry_ptr ss, int index);

/* void o2_find_proc_services(process_info_ptr proc, int tags_flag);
// results are returned in these dynamic arrays:
dyn_array o2_services_rslt;
dyn_array o2_taps_rslt;
// using these types:
typedef struct service_data {
    services_entry_ptr ss; // services node containing this service
    o2_info_ptr s; // service provider
    int i; // index of service provider in ss->services
} service_data, *service_data_ptr;

typedef struct tap_data {
    services_entry_ptr ss; // services node containing this service
    tap_info_ptr tap;
} tap_data, *tap_data_ptr;
*/

int o2_embedded_msgs_deliver(o2_msg_data_ptr msg, int tcp_flag);

/**
 * Deliver a message immediately and locally. If service is given,
 * the caller must check to see if this is a bundle and call 
 * deliver_embedded_msgs() if so.
 *
 * @param msg the message data to deliver
 * @param tcp_flag tells whether to send via tcp; this is only used
 *                 when the message is a bundle and elements of the
 *                 bundle are addressed to remote services
 * @param service is the service to which the message is addressed.
 *                If the service is unknown, pass NULL.
 */
void o2_msg_data_deliver(o2_msg_data_ptr msg, int tcp_flag,
                         o2_info_ptr service, services_entry_ptr services);

void o2_node_finish(node_entry_ptr node);

o2string o2_heapify(const char *path);

/**
 *  Initialize a table entry.
 *
 *  @param node The path tree entry to be initialized.
 *  @param key The key (name) of this entry. key is owned by the caller and
 *         a copy is made and owned by the node.
 *
 *  @return O2_SUCCESS or O2_FAIL
 */
node_entry_ptr o2_node_initialize(node_entry_ptr node, const char *key);

/**
 *  Look up the key in certain table and return the pointer to the entry.
 *
 *  @param dict  The table that the entry is supposed to be in.
 *  @param key   The key.
 *
 *  @return The address of the pointer to the entry.
 */
o2_entry_ptr *o2_lookup(node_entry_ptr dict, o2string key);

int o2_remove_remote_process(process_info_ptr info);

services_entry_ptr o2_insert_new_service(o2string service_name,
                                         services_entry_ptr *services);

#ifdef __cplusplus
}
#endif

#endif /* o2_search_h */
