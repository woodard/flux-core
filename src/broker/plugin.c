/*****************************************************************************\
 *  Copyright (c) 2014 Lawrence Livermore National Security, LLC.  Produced at
 *  the Lawrence Livermore National Laboratory (cf, AUTHORS, DISCLAIMER.LLNS).
 *  LLNL-CODE-658032 All rights reserved.
 *
 *  This file is part of the Flux resource manager framework.
 *  For details, see https://github.com/flux-framework.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the license, or (at your option)
 *  any later version.
 *
 *  Flux is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *  See also:  http://www.gnu.org/licenses/
\*****************************************************************************/

/* plugin.c - broker plugin interface */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/param.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <fnmatch.h>

#include <json/json.h>
#include <czmq.h>

#include "log.h"
#include "zmsg.h"
#include "zdump.h"
#include "zconnect.h"
#include "jsonutil.h"
#include "xzmalloc.h"

#include "flux.h"
#include "handle.h"

#include "plugin.h"


typedef struct {
    int request_tx;
    int request_rx;
    int svc_tx;
    int svc_rx;
    int event_tx;
    int event_rx;
} plugin_stats_t;

#define PLUGIN_MAGIC    0xfeefbe01
struct plugin_ctx_struct {
    int magic;
    void *zs_request;
    void *zs_svc[2]; /* for handling requests 0=plugin, 1=cmbd */
    void *zs_evin;
    char *svc_uri;
    zuuid_t *uuid;
    pthread_t t;
    mod_main_f *main;
    plugin_stats_t stats;
    zloop_t *zloop;
    zlist_t *deferred_responses;
    void *zctx;
    flux_sec_t sec;
    flux_t h;
    const char *name;
    void *dso;
    int size;
    char *digest;
    zhash_t *args;
    int rank;
    bool reactor_stop;
    int reactor_rc;
};

static void plugin_reactor_stop (void *impl, int rc);

#if CZMQ_VERSION_MAJOR < 2
#error Need CZMQ v2 timer API
#endif

#define ZLOOP_RETURN(p) \
    return ((p)->reactor_stop ? (-1) : (0))

static const struct flux_handle_ops plugin_handle_ops;


/**
 ** flux_t implementation
 **/

static int plugin_request_sendmsg (void *impl, zmsg_t **zmsg)
{
    plugin_ctx_t p = impl;
    int rc;

    assert (p->magic == PLUGIN_MAGIC);
    rc = zmsg_send (zmsg, p->zs_request);
    p->stats.request_tx++;
    return rc;
}

static zmsg_t *plugin_request_recvmsg (void *impl, bool nb)
{
    plugin_ctx_t p = impl;
    zmsg_t *zmsg;

    assert (p->magic == PLUGIN_MAGIC);
    zmsg = zmsg_recv (p->zs_svc[0]); /* FIXME: ignores nb flag */
    if (zmsg && zmsg_content_size (zmsg) == 0) { /* EOF */
        plugin_reactor_stop (p, 0);
        zmsg_destroy (&zmsg);
    }
    return zmsg;
}


static int plugin_response_sendmsg (void *impl, zmsg_t **zmsg)
{
    int rc;
    plugin_ctx_t p = impl;

    assert (p->magic == PLUGIN_MAGIC);
    rc = zmsg_send (zmsg, p->zs_svc[0]);
    p->stats.svc_tx++;
    return rc;
}

static zmsg_t *plugin_response_recvmsg (void *impl, bool nb)
{
    plugin_ctx_t p = impl;
    assert (p->magic == PLUGIN_MAGIC);
    return zmsg_recv (p->zs_request); /* FIXME: ignores nb flag */
}

static int plugin_response_putmsg (void *impl, zmsg_t **zmsg)
{
    plugin_ctx_t p = impl;
    assert (p->magic == PLUGIN_MAGIC);
    if (zlist_append (p->deferred_responses, *zmsg) < 0)
        oom ();
    *zmsg = NULL;
    return 0;
}

static zmsg_t *plugin_event_recvmsg (void *impl, bool nb)
{
    plugin_ctx_t p = impl;
    assert (p->magic == PLUGIN_MAGIC);
    return zmsg_recv (p->zs_evin); /* FIXME: ignores nb flag */
}

static int plugin_event_subscribe (void *impl, const char *topic)
{
    plugin_ctx_t p = impl;
    char *s = topic ? (char *)topic : "";
    assert (p->magic == PLUGIN_MAGIC);
    return zmq_setsockopt (p->zs_evin, ZMQ_SUBSCRIBE, s, strlen (s));
}

static int plugin_event_unsubscribe (void *impl, const char *topic)
{
    plugin_ctx_t p = impl;
    char *s = topic ? (char *)topic : "";
    assert (p->magic == PLUGIN_MAGIC);
    return zmq_setsockopt (p->zs_evin, ZMQ_UNSUBSCRIBE, s, strlen (s));
}

static int plugin_rank (void *impl)
{
    plugin_ctx_t p = impl;
    assert (p->magic == PLUGIN_MAGIC);
    return p->rank;
}

static zctx_t *plugin_get_zctx (void *impl)
{
    plugin_ctx_t p = impl;
    assert (p->magic == PLUGIN_MAGIC);
    return p->zctx;
}

static flux_sec_t plugin_get_sec (void *impl)
{
    plugin_ctx_t p = impl;
    assert (p->magic == PLUGIN_MAGIC);
    return p->sec;
}

static int plugin_reactor_start (void *impl)
{
    plugin_ctx_t p = impl;
    zloop_start (p->zloop);
    return p->reactor_rc;
};

static void plugin_reactor_stop (void *impl, int rc)
{
    plugin_ctx_t p = impl;
    p->reactor_stop = true;
    p->reactor_rc = rc;
}

static int fd_cb (zloop_t *zl, zmq_pollitem_t *item, plugin_ctx_t p)
{
    if (flux_handle_event_fd (p->h, item->fd, item->revents) < 0)
        plugin_reactor_stop (p, -1);
    ZLOOP_RETURN(p);
}

static int plugin_reactor_fd_add (void *impl, int fd, short events)
{
    plugin_ctx_t p = impl;
    zmq_pollitem_t item = { .fd = fd, .events = events };
#if ZMQ_IGNERR
    item.events |= ZMQ_IGNERR;
#endif
    if (zloop_poller (p->zloop, &item, (zloop_fn *)fd_cb, p) < 0)
        return -1;
#ifndef ZMQ_IGNERR
    zloop_set_tolerant (p->zloop, &item);
#endif
    return 0;
}

static void plugin_reactor_fd_remove (void *impl, int fd, short events)
{
    plugin_ctx_t p = impl;
    zmq_pollitem_t item = { .fd = fd, .events = events };

    zloop_poller_end (p->zloop, &item); /* FIXME: 'events' are ignored */
}

static int zs_cb (zloop_t *zl, zmq_pollitem_t *item, plugin_ctx_t p)
{
    if (flux_handle_event_zs (p->h, item->socket, item->revents) < 0)
        plugin_reactor_stop (p, -1);
    ZLOOP_RETURN(p);
}

static int plugin_reactor_zs_add (void *impl, void *zs, short events)
{
    plugin_ctx_t p = impl;
    zmq_pollitem_t item = { .socket = zs, .events = events };

    return zloop_poller (p->zloop, &item, (zloop_fn *)zs_cb, p);
}

static void plugin_reactor_zs_remove (void *impl, void *zs, short events)
{
    plugin_ctx_t p = impl;
    zmq_pollitem_t item = { .socket = zs, .events = events };

    zloop_poller_end (p->zloop, &item); /* FIXME: 'events' are ignored */
}

static int tmout_cb (zloop_t *zl, int timer_id, plugin_ctx_t p)
{
    if (flux_handle_event_tmout (p->h, timer_id) < 0)
        plugin_reactor_stop (p, -1);
    ZLOOP_RETURN(p);
}

static int plugin_reactor_tmout_add (void *impl, unsigned long msec, bool oneshot)
{
    plugin_ctx_t p = impl;
    int times = oneshot ? 1 : 0;

    return zloop_timer (p->zloop, msec, times, (zloop_timer_fn *)tmout_cb, p);
}

static void plugin_reactor_tmout_remove (void *impl, int timer_id)
{
    plugin_ctx_t p = impl;

    zloop_timer_end (p->zloop, timer_id);
}

/**
 ** end of handle implementation
 **/

static int ping_req_cb (flux_t h, int typemask, zmsg_t **zmsg, void *arg)
{
    json_object *o;
    char *s = NULL;
    int rc = 0;

    if (flux_msg_decode (*zmsg, NULL, &o) < 0 || o == NULL) {
        err ("%s: protocol error", __FUNCTION__);
        goto done; /* reactor continues */
    }
    /* Route string will not include the endpoints.
     * On arrival here, uuid of dst plugin has been stripped.
     * The '1' arg to zdump_routestr strips the uuid of the sender.
     */
    s = zdump_routestr (*zmsg, 1);
    util_json_object_add_string (o, "route", s);
    if (flux_respond (h, zmsg, o) < 0) {
        err ("%s: flux_respond", __FUNCTION__);
        rc = -1; /* reactor terminates */
        goto done;
    }
done:
    if (o)
        json_object_put (o);
    if (s)
        free (s);
    if (*zmsg)
        zmsg_destroy (zmsg);
    return rc;
}

static int stats_cb (flux_t h, int typemask, zmsg_t **zmsg, void *arg)
{
    plugin_ctx_t p = arg;
    json_object *o = NULL;
    int rc = 0;
    char *tag = NULL;

    if (flux_msg_decode (*zmsg, &tag, &o) < 0) {
        flux_log (p->h, LOG_ERR, "%s: error decoding message", __FUNCTION__);
        goto done;
    }
    if (fnmatch ("*.get", tag, 0) == 0) {
        util_json_object_add_int (o, "#request (tx)", p->stats.request_tx);
        util_json_object_add_int (o, "#request (rx)", p->stats.request_rx);
        util_json_object_add_int (o, "#svc (tx)",     p->stats.svc_tx);
        util_json_object_add_int (o, "#svc (rx)",     p->stats.svc_rx);
        util_json_object_add_int (o, "#event (tx)",   p->stats.event_tx);
        util_json_object_add_int (o, "#event (rx)",   p->stats.event_rx);
        if (flux_respond (h, zmsg, o) < 0) {
            err ("%s: flux_respond", __FUNCTION__);
            rc = -1;
            goto done;
        }
    } else if (fnmatch ("*.clear", tag, 0) == 0) {
        memset (&p->stats, 0, sizeof (p->stats));
        if (typemask & FLUX_MSGTYPE_REQUEST) {
            if (flux_respond_errnum (h, zmsg, 0) < 0) {
                err ("%s: flux_respond_errnum", __FUNCTION__);
                rc = -1;
                goto done;
            }
        }
    } else {
        flux_log (p->h, LOG_ERR, "%s: %s: unknown tag", __FUNCTION__, tag);
    }
done:
    if (tag)
        free (tag);
    if (o)
        json_object_put (o);
    if (*zmsg)
        zmsg_destroy (zmsg);
    return rc;
}

static int rusage_cb (flux_t h, int typemask, zmsg_t **zmsg, void *arg)
{
    plugin_ctx_t p = arg;
    json_object *request = NULL;
    json_object *response = NULL;
    int rc = 0;
    struct rusage usage;

    if (flux_msg_decode (*zmsg, NULL, &request) < 0 || request == NULL) {
        flux_log (p->h, LOG_ERR, "%s: error decoding message", __FUNCTION__);
        goto done;
    }
    if (getrusage (RUSAGE_THREAD, &usage) < 0) {
        if (flux_respond_errnum (h, zmsg, errno) < 0) {
            err ("%s: flux_respond_errnum", __FUNCTION__);
            rc = -1;
            goto done;
        }
        goto done;
    }
    response = rusage_to_json (&usage);
    if (flux_respond (h, zmsg, response) < 0) {
        err ("%s: flux_respond", __FUNCTION__);
        rc = -1;
        goto done;
    }
done:
    if (request)
        json_object_put (request);
    if (response)
        json_object_put (response);
    if (*zmsg)
        zmsg_destroy (zmsg);
    return rc;
}

static void plugin_handle_response (plugin_ctx_t p, zmsg_t *zmsg)
{
    p->stats.request_rx++;

    if (zmsg) {
        if (flux_handle_event_msg (p->h, FLUX_MSGTYPE_RESPONSE, &zmsg) < 0) {
            plugin_reactor_stop (p, -1);
            goto done;
        }
    }
done:
    if (zmsg)
        zmsg_destroy (&zmsg);
}

/* Process any responses received during synchronous request-reply handling.
 * Call this after every plugin callback that may have invoked one of the
 * synchronous request-reply functions.
 */
static void plugin_handle_deferred_responses (plugin_ctx_t p)
{
    zmsg_t *zmsg;

    while ((zmsg = zlist_pop (p->deferred_responses)))
        plugin_handle_response (p, zmsg);
}

/* Handle a response.
 */
static int request_cb (zloop_t *zl, zmq_pollitem_t *item, plugin_ctx_t p)
{
    zmsg_t *zmsg = zmsg_recv (p->zs_request);

    plugin_handle_response (p, zmsg);
    plugin_handle_deferred_responses (p);
    ZLOOP_RETURN(p);
}

/* Handle a request.
 */
static int svc_cb (zloop_t *zl, zmq_pollitem_t *item, plugin_ctx_t p)
{
    zmsg_t *zmsg = zmsg_recv (p->zs_svc[0]);

    p->stats.svc_rx++;

    if (zmsg && zmsg_content_size (zmsg) == 0) { /* EOF */
        plugin_reactor_stop (p, 0);
        goto done;
    }
    if (zmsg) {
        if (flux_handle_event_msg (p->h, FLUX_MSGTYPE_REQUEST, &zmsg) < 0) {
            plugin_reactor_stop (p, -1);
            goto done;
        }
    }
    if (zmsg) {
        if (flux_respond_errnum (p->h, &zmsg, ENOSYS) < 0) {
            err ("%s: flux_respond_errnum", __FUNCTION__);
            goto done;
        }
    }
    plugin_handle_deferred_responses (p);
done:
    if (zmsg)
        zmsg_destroy (&zmsg);
    ZLOOP_RETURN(p);
}

static int event_cb (zloop_t *zl, zmq_pollitem_t *item, plugin_ctx_t p)
{
    zmsg_t *zmsg = zmsg_recv (p->zs_evin);

    p->stats.event_rx++;

    if (zmsg) {
        if (flux_handle_event_msg (p->h, FLUX_MSGTYPE_EVENT, &zmsg) < 0) {
            plugin_reactor_stop (p, -1);
            goto done;
        }
    }
    plugin_handle_deferred_responses (p);
done:
    if (zmsg)
        zmsg_destroy (&zmsg);
    ZLOOP_RETURN(p);
}

static zloop_t * plugin_zloop_create (plugin_ctx_t p)
{
    int rc;
    zloop_t *zl;
    zmq_pollitem_t zp = { .events = ZMQ_POLLIN, .revents = 0, .fd = -1 };

    if (!(zl = zloop_new ()))
        err_exit ("zloop_new");

    zp.socket = p->zs_request;
    if ((rc = zloop_poller (zl, &zp, (zloop_fn *) request_cb, (void *) p)) != 0)
        err_exit ("zloop_poller: rc=%d", rc);
    zp.socket = p->zs_svc[0];
    if ((rc = zloop_poller (zl, &zp, (zloop_fn *) svc_cb, (void *) p)) != 0)
        err_exit ("zloop_poller: rc=%d", rc);
    zp.socket = p->zs_evin;
    if ((rc = zloop_poller (zl, &zp, (zloop_fn *) event_cb, (void *) p)) != 0)
        err_exit ("zloop_poller: rc=%d", rc);

    return (zl);
}

static void register_event (plugin_ctx_t p, char *name, FluxMsgHandler cb)
{
    char *s;

    if (asprintf (&s, "%s.%s", p->name, name) < 0)
        oom ();
    if (flux_msghandler_add (p->h, FLUX_MSGTYPE_EVENT, s, cb, p) < 0)
        err_exit ("%s: flux_msghandler_add", p->name);
    /* Trim a glob wildcard off the end to form a subscription string.
     * 0MQ subscriptions are a rooted substring match.
     * Globs in other places are fatal.
     */
    if (s[strlen (s) - 1] == '*')
        s[strlen (s) - 1] = '\0';
    if (strchr (s, '*'))
        err_exit ("%s: cant deal with '%s' subscription", __FUNCTION__, name);
    if (flux_event_subscribe (p->h, s) < 0)
        err_exit ("%s: flux_event_subscribe %s", p->name, s);
    free (s);
}

static void register_request (plugin_ctx_t p, char *name, FluxMsgHandler cb)
{
    char *s;

    if (asprintf (&s, "%s.%s", p->name, name) < 0)
        oom ();
    if (flux_msghandler_add (p->h, FLUX_MSGTYPE_REQUEST, s, cb, p) < 0)
        err_exit ("%s: flux_msghandler_add %s", p->name, s);
    free (s);
}

static void *plugin_thread (void *arg)
{
    plugin_ctx_t p = arg;
    sigset_t signal_set;
    int errnum;

    /* block all signals */
    if (sigfillset (&signal_set) < 0)
        err_exit ("sigfillset");
    if ((errnum = pthread_sigmask (SIG_BLOCK, &signal_set, NULL)) != 0)
        errn_exit (errnum, "pthread_sigmask");

    p->zloop = plugin_zloop_create (p);
    if (p->zloop == NULL)
        err_exit ("%s: plugin_zloop_create", p->name);

    /* Register callbacks for "internal" methods.
     * These can be overridden in p->ops->main() if desired.
     */
    register_request (p, "ping",    ping_req_cb);
    register_request (p, "stats.*", stats_cb);
    register_request (p, "rusage", rusage_cb);
    register_event   (p, "stats.*", stats_cb);

    if (p->main(p->h, p->args) < 0) {
        err ("%s: mod_main returned error", p->name);
        goto done;
    }
done:
    zloop_destroy (&p->zloop);
    zstr_send (p->zs_svc[0], ""); /* EOF */

    return NULL;
}

const char *plugin_name (plugin_ctx_t p)
{
    return p->name;
}

const char *plugin_uuid (plugin_ctx_t p)
{
    return zuuid_str (p->uuid);
}

void *plugin_sock (plugin_ctx_t p)
{
    return p->zs_svc[1];
}

const char *plugin_digest (plugin_ctx_t p)
{
    return p->digest;
}

int plugin_size (plugin_ctx_t p)
{
    return p->size;
}

void plugin_destroy (plugin_ctx_t p)
{
    int errnum;
    zmsg_t *zmsg;

    if (p->t) {
        errnum = pthread_join (p->t, NULL);
        if (errnum)
            errn_exit (errnum, "pthread_join");
    }

    flux_handle_destroy (&p->h);

    zsocket_destroy (p->zctx, p->zs_evin);
    zsocket_destroy (p->zctx, p->zs_svc[0]);
    zsocket_destroy (p->zctx, p->zs_svc[1]);
    zsocket_destroy (p->zctx, p->zs_request);

    while ((zmsg = zlist_pop (p->deferred_responses)))
        zmsg_destroy (&zmsg);
    zlist_destroy (&p->deferred_responses);

    dlclose (p->dso);
    zuuid_destroy (&p->uuid);
    free (p->svc_uri);
    free (p->digest);

    free (p);
}

void plugin_stop (plugin_ctx_t p)
{
    zstr_send (p->zs_svc[1], ""); /* EOF */
}

void plugin_start (plugin_ctx_t p)
{
    int errnum;
    errnum = pthread_create (&p->t, NULL, plugin_thread, p);
    if (errnum)
        errn_exit (errnum, "pthread_create");
}

char *plugin_getstring (const char *path, const char *name)
{
    void *dso;
    char *s = NULL;
    const char **np;

    if (access (path, F_OK) < 0)
        return NULL;
    dlerror ();
    if (!(dso = dlopen (path, RTLD_NOW | RTLD_LOCAL))) {
        msg ("%s", dlerror ());
        goto done;
    }
    if (!(np = dlsym (dso, name)) || !*np) {
        msg ("%s", dlerror ());
        goto done;
    }
    s = xstrdup (*np);
done:
    if (dso)
        dlclose (dso);
    return s;
}

plugin_ctx_t plugin_create (flux_t h, const char *path, zhash_t *args)
{
    plugin_ctx_t p;
    void *dso;
    const char **mod_namep;
    mod_main_f *mod_main;
    zfile_t *zf;

    dlerror ();
    if (!(dso = dlopen (path, RTLD_NOW | RTLD_LOCAL))) {
        msg ("%s", dlerror ());
        errno = ENOENT;
        return NULL;
    }
    mod_main = dlsym (dso, "mod_main");
    mod_namep = dlsym (dso, "mod_name");
    if (!mod_main || !mod_namep || !*mod_namep) {
        err ("%s: mod_main or mod_name undefined", path);
        dlclose (dso);
        errno = ENOENT;
        return NULL;
    }

    p = xzmalloc (sizeof (*p));
    p->magic = PLUGIN_MAGIC;
    p->zctx = flux_get_zctx (h);
    p->sec = flux_get_sec (h);
    p->args = args;
    p->main = mod_main;
    p->dso = dso;
    zf = zfile_new (NULL, path);
    p->digest = xstrdup (zfile_digest (zf));
    p->size = (int)zfile_cursize (zf);
    zfile_destroy (&zf);
    p->name = *mod_namep;
    if (asprintf (&p->svc_uri, "inproc://svc-%s", p->name) < 0)
        oom ();
    if (!(p->uuid = zuuid_new ()))
        oom ();
    p->rank = flux_rank (h);
    if (!(p->deferred_responses = zlist_new ()))
        oom ();

    p->h = flux_handle_create (p, &plugin_handle_ops, 0);
    flux_log_set_facility (p->h, p->name);

    /* connect sockets in the parent, then use them in the thread */
    zconnect (p->zctx, &p->zs_request, ZMQ_DEALER, REQUEST_URI, -1,
              zuuid_str (p->uuid));
    zbind (p->zctx, &p->zs_svc[1], ZMQ_PAIR, p->svc_uri, -1);
    zconnect (p->zctx, &p->zs_svc[0], ZMQ_PAIR, p->svc_uri, -1, NULL);
    zconnect (p->zctx, &p->zs_evin,  ZMQ_SUB, EVENT_URI, 0, NULL);

    return p;
}


static const struct flux_handle_ops plugin_handle_ops = {
    .request_sendmsg = plugin_request_sendmsg,
    .request_recvmsg = plugin_request_recvmsg,
    .response_sendmsg = plugin_response_sendmsg,
    .response_recvmsg = plugin_response_recvmsg,
    .response_putmsg = plugin_response_putmsg,
    .event_recvmsg = plugin_event_recvmsg,
    .event_subscribe = plugin_event_subscribe,
    .event_unsubscribe = plugin_event_unsubscribe,
    .rank = plugin_rank,
    .get_zctx = plugin_get_zctx,
    .get_sec = plugin_get_sec,
    .reactor_stop = plugin_reactor_stop,
    .reactor_start = plugin_reactor_start,
    .reactor_fd_add = plugin_reactor_fd_add,
    .reactor_fd_remove = plugin_reactor_fd_remove,
    .reactor_zs_add = plugin_reactor_zs_add,
    .reactor_zs_remove = plugin_reactor_zs_remove,
    .reactor_tmout_add = plugin_reactor_tmout_add,
    .reactor_tmout_remove = plugin_reactor_tmout_remove,
};

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */