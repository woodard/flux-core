#ifndef _FLUX_CORE_MODULE_H
#define _FLUX_CORE_MODULE_H

#include <json.h>
#include <czmq.h>

#include "handle.h"

/* Manipulate comms modules.
 * Use rank=-1 for local.
 */
enum {
    FLUX_MOD_FLAGS_MANAGED = 1,
};
int flux_rmmod (flux_t h, int rank, const char *name, int flags);
json_object *flux_lsmod (flux_t h, int rank, const char *target);
int flux_insmod (flux_t h, int rank, const char *path, int flags,
                 json_object *args);

/* Comms modules must define  MOD_NAME and mod_main().
 */
typedef int (mod_main_f)(flux_t h, zhash_t *args);
extern mod_main_f mod_main;
#define MOD_NAME(x) const char *mod_name = x

/* Get the name of a module given its path, or NULL on failure.
 * Caller must free the returned name.
 */
char *flux_modname (const char *modpath);

/* Search a colon-separated list of directories for a .so file
 * with the requested module name and return its path, or NULL on failure.
 * Caller must free the returned path.
 */
char *flux_modfind (const char *searchpath, const char *modname);


#endif /* !FLUX_CORE_MODULE_H */

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
