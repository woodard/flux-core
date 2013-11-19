#ifndef PLUGIN_H
#define PLUGIN_H

#include "flux.h"

/* Plugins will be connected to these well-known shared memory zmq sockets.
 */
#define UPREQ_URI           "inproc://upreq"
#define DNREQ_URI           "inproc://dnreq"
#define DNEV_OUT_URI        "inproc://evout"
#define DNEV_IN_URI         "inproc://evin"
#define SNOOP_URI           "inproc://snoop"

typedef enum {
    ZMSG_REQUEST, ZMSG_RESPONSE, ZMSG_EVENT, ZMSG_SNOOP }
zmsg_type_t;

/* A plugin defines 'const struct plugin_ops ops = {...}' containing
 * its implementations of one or more of the plugin operations.
 */
struct plugin_ops {
    void (*timeout)(flux_t h);
    void (*recv)(flux_t h, zmsg_t **zmsg, zmsg_type_t type);
    int (*init)(flux_t h, zhash_t *args);
    void (*fini)(flux_t h);
};

typedef struct plugin_ctx_struct *plugin_ctx_t;

/* Load the specified plugin by 'name' and return a handle for it,
 * or NULL on failure.  We dlopen() the file "./<name>srv.so".
 * 'id' is a session-wide unique socket id for this instance of the plugin,
 * used to form the return address when the plugin a request on its dealer
 * socket.  'args' is a hash of key-value pairs that may be NULL, or may
 * be used to pass arguments to the plugins's ops->init() function.
 */
plugin_ctx_t plugin_load (zctx_t *zctx, char *name, char *id, zhash_t *args);

/* Unload a plugin by handle.
 * (FIXME: This is not used yet and is a work in progress)
 */
void plugin_unload (plugin_ctx_t p);

#endif /* !PLUGIN_H */

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
