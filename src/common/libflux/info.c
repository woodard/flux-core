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

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <errno.h>
#include <stdbool.h>

#include "info.h"
#include "rpc.h"

#include "src/common/libutil/shortjson.h"
#include "src/common/libutil/xzmalloc.h"

int flux_info (flux_t h, uint32_t *rankp, uint32_t *sizep, int *arityp)
{
    flux_rpc_t *r = NULL;
    JSON out = NULL;
    const char *json_str;
    int arity, rank, size;
    int ret = -1;

    if (!(r = flux_rpc (h, "cmb.info", NULL, FLUX_NODEID_ANY, 0)))
        goto done;
    if (flux_rpc_get (r, NULL, &json_str) < 0)
        goto done;
    if (!(out = Jfromstr (json_str))
            || !Jget_int (out, "arity", &arity)
            || !Jget_int (out, "rank", &rank)
            || !Jget_int (out, "size", &size)) {
        errno = EPROTO;
        goto done;
    }
    if (rankp)
        *rankp = rank;
    if (sizep)
        *sizep = size;
    if (arityp)
        *arityp = arity;
    ret = 0;
done:
    Jput (out);
    if (r)
        flux_rpc_destroy (r);
    return ret;
}

/* The size is not cacheable per RFC 3.
 * However, allow the size to be faked to facilitate 'loop' testing.
 */
int flux_size (flux_t h)
{
    uint32_t size, *sizep;
    if ((sizep = flux_aux_get (h, "flux::size")))
        return *sizep;
    if (flux_info (h, NULL, &size, NULL) < 0)
        return -1;
    return size;
}

/* The rank is cacheable, since it never changes per RFC 3.
 * In addition, allow the rank to be faked to facilitate 'loop' testing.
 */
int flux_rank (flux_t h)
{
    uint32_t *rank = flux_aux_get (h, "flux::rank");
    if (!rank) {
        if (!(rank = malloc (sizeof (*rank)))) {
            errno = ENOMEM;
            return -1;
        }
        if (flux_info (h, rank, NULL, NULL) < 0)
            return -1;
        flux_aux_set (h, "flux::rank", rank, free);
    }
    return *rank;
}


/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
