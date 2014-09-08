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

/* flux-snoop.c - flux snoop subcommand */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <getopt.h>
#include <json/json.h>
#include <assert.h>
#include <libgen.h>

#include <zmq.h>
#include <czmq.h>

#include "zdump.h"
#include "log.h"

#include "flux.h"

#define OPTIONS "hanN:vl"
static const struct option longopts[] = {
    {"help",       no_argument,        0, 'h'},
    {"all",        no_argument,        0, 'a'},
    {"no-security",no_argument,        0, 'n'},
    {"verbose",    no_argument,        0, 'v'},
    {"long",       no_argument,        0, 'l'},
    {"session-name",required_argument, 0, 'N'},
    { 0, 0, 0, 0 },
};

static char *suppressed[] = { "cmb.info", "log.msg", "cmb.pub" };

void usage (void)
{
    fprintf (stderr, 
"Usage: flux-snoop OPTIONS [topic [topic...]]\n"
"  -a,--all               Do not suppress cmb.info, log.msg, cmb.pub\n"
"  -n,--no-security       Try to connect without CURVE security\n"
"  -v,--verbose           Verbose connect output\n"
"  -l,--long              Display long message format\n"
"  -N,--session-name NAME Set session name (default flux)\n"
);
    exit (1);
}

static void *connect_snoop (zctx_t *zctx, flux_sec_t sec, const char *uri);
static int snoop_cb (zloop_t *zloop, zmq_pollitem_t *item, void *arg);
static int zmon_cb (zloop_t *zloop, zmq_pollitem_t *item, void *arg);

static bool aopt = false;
static bool lopt = false;

int main (int argc, char *argv[])
{
    flux_t h;
    int ch;
    char *uri = NULL;
    bool vopt = false;
    bool nopt = false;
    char *session = "flux";
    zctx_t *zctx;
    zlist_t *subs;
    void *s;
    zloop_t *zloop;
    zmq_pollitem_t zp;
    flux_sec_t sec;
    int rank = -1;

    log_init ("flux-snoop");

    if (!(subs = zlist_new ()))
        oom ();
    while ((ch = getopt_long (argc, argv, OPTIONS, longopts, NULL)) != -1) {
        switch (ch) {
            case 'h': /* --help */
                usage ();
                break;
            case 'a': /* --all */
                aopt = true;
                break;
            case 'l': /* --long */
                lopt = true;
                break;
            case 'n': /* --no-security */
                nopt = true;
                break;
            case 'v': /* --verbose */
                vopt = true;
                break;
            case 'N': /* --session-name NAME */
                session = optarg;;
                break;
            default:
                usage ();
                break;
        }
    }
    while (optind < argc) {
        if (zlist_append (subs, argv[optind++]) < 0)
            oom ();
    }
    if (!(h = flux_api_open ()))
        err_exit ("flux_api_open");
    if (!(uri = flux_getattr (h, rank, "cmbd-snoop-uri")))
        err_exit ("cmbd-snoop-uri");

    /* N.B. flux_get_zctx () is not implemented for the API socket since
     * it has no internal zctx (despite supporting the flux reactor).
     */
    if (!(zctx = zctx_new ()))
        err_exit ("zctx_new");
    zctx_set_linger (zctx, 5);

    /* N.B. We use the zloop reactor and handle disconnects via zmonitor.
     * We must handle disconnects, since the default zmq "hidden reconnect"
     * behavior doesn't work across a cmbd restart, where the dynamically
     * assigned snoop URI may change.
     */
    if (!(zloop = zloop_new ()))
        oom ();

    /* Initialize security ctx.
     */
    if (!(sec = flux_sec_create ()))
        err_exit ("flux_sec_create");
    if (nopt) {
        if (flux_sec_disable (sec, FLUX_SEC_TYPE_ALL) < 0)
            err_exit ("flux_sec_disable");
        msg ("Security is disabled");
    }
    if (flux_sec_zauth_init (sec, zctx, session) < 0)
        msg_exit ("flux_sec_zinit: %s", flux_sec_errstr (sec));

    /* Connect to the snoop socket and subscribe to topics of interest.
     */
    if (vopt)
        msg ("connecting to %s...", uri);

    if (!(s = connect_snoop (zctx, sec, uri)))
        err_exit ("%s", uri);
    zp.socket = s;
    zp.events = ZMQ_POLLIN;
    if (zloop_poller (zloop, &zp, snoop_cb, NULL) < 0)
        err_exit ("zloop_poller");

    if (zlist_size (subs) == 0)
        zsocket_set_subscribe (s, "");
    else {
        char *sub; /* actual argv element, do not free */
        while ((sub = zlist_pop (subs)))
            zsocket_set_subscribe (s, sub);
    }

#if CZMQ_VERSION_MAJOR >= 2 && ZMQ_VERSION_MAJOR >= 4
    zmonitor_t *zmon;
    if (!(zmon = zmonitor_new (zctx, s, ZMQ_EVENT_DISCONNECTED)))
        err_exit ("zmonitor_new");
    if (vopt)
        zmonitor_set_verbose (zmon, true);
    zp.socket = zmonitor_socket (zmon);
#else
#define DEFAULT_ZMON_URI    "inproc://monitor.snoop"
    if (zmq_socket_monitor (s, DEFAULT_ZMON_URI, ZMQ_EVENT_DISCONNECTED) < 0)
        err_exit ("zmq_socket_monitor");
    if (!(zp.socket = zsocket_new  (zctx, ZMQ_PAIR)))
        err_exit ("zsocket_new");
    if (zsocket_connect (zp.socket, DEFAULT_ZMON_URI) < 0)
        err_exit ("zsocket_connect %s", DEFAULT_ZMON_URI);
#endif
    zp.events = ZMQ_POLLIN;
    if (zloop_poller (zloop, &zp, zmon_cb, NULL) < 0)
        err_exit ("zloop_poller");

    if (zloop_start (zloop) < 0)
        err_exit ("zloop_start");
    if (vopt)
        msg ("disconnecting");

#if CZMQ_VERSION_MAJOR >= 2 && ZMQ_VERSION_MAJOR >= 4
    zmonitor_destroy (&zmon);
#endif

    zloop_destroy (&zloop);
    zctx_destroy (&zctx); /* destroys 's' and 'zp.socket' */

    zlist_destroy (&subs);
    free (uri);
    flux_api_close (h);
    log_fini ();
    return 0;
}

static void *connect_snoop (zctx_t *zctx, flux_sec_t sec, const char *uri)
{
    void *s;

    if (!(s = zsocket_new (zctx, ZMQ_SUB)))
        err_exit ("zsocket_new");
    if (flux_sec_csockinit (sec, s) < 0)
        msg_exit ("flux_sec_csockinit: %s", flux_sec_errstr (sec));
    if (zsocket_connect (s, "%s", uri) < 0)
        err_exit ("%s", uri);

    return s;
}

static bool suppress (const char *tag)
{
    int i;

    for (i = 0; i < sizeof (suppressed)/sizeof (suppressed[0]); i++)
        if (!strcmp (tag, suppressed[i]))
            return true;
    return false;
}

/* The snoop socket includes two extra header frames:
 * First the tag frame, stripped of any node! prefix so subscriptions work.
 * Second, the message type as a stringified integer.
 */
static int snoop_cb (zloop_t *zloop, zmq_pollitem_t *item, void *arg)
{
    void *zs = item->socket;
    zmsg_t *zmsg;

    if ((zmsg = zmsg_recv (zs))) {
        char *tag = zmsg_popstr (zmsg);
        char *typestr = zmsg_popstr (zmsg);
        int type;

        if (tag && typestr) {
            if (aopt || !suppress (tag)) {
                if (lopt) {
                    zmsg_dump (zmsg);
                } else {
                    type = strtoul (typestr, NULL, 10);
                    zdump_fprint (stderr, zmsg, flux_msgtype_shortstr (type));
                }
            }
        }
        if (tag)
            free (tag);
        if (typestr)
            free (typestr);
        zmsg_destroy (&zmsg);
    }
    return 0;
}

static int zmon_cb (zloop_t *zloop, zmq_pollitem_t *item, void *arg)
{
    zmsg_t *zmsg;
    int event = 0;

    if ((zmsg = zmsg_recv (item->socket))) {
#if ZMQ_VERSION_MAJOR >= 4
        char *s = zmsg_popstr (zmsg);
        if (s) {
            event = strtoul (s, NULL, 10);
            free (s);
        }
#else
        zmq_event_t ev;
        zframe_t *zf = zmsg_first (zmsg);
        if (zf && zframe_size (zf) == sizeof (ev)) {
            memcpy (&ev, zframe_data (zf), zframe_size (zf));
            event = ev.event;
        }
#endif
        if (event == ZMQ_EVENT_DISCONNECTED)
            msg_exit ("lost connection");
        zmsg_destroy (&zmsg);
    }
    return 0;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */