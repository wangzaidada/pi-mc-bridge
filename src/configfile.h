/* configfile.h - INI-subset configuration file loader.
 *
 * Format (design D5):
 *   [global]                 -- defaults, overridden by env then CLI
 *   [trigger:<name>]         -- one trigger per section
 *   key = value              -- '#' or ';' at the start of a line is a comment
 *   keywords = a, b, c       -- comma-separated list
 *
 * Values are trimmed of surrounding whitespace. An unrecognized key, an
 * unrecognized section, or a trigger without keywords is a hard error: a
 * misspelled key must not silently disable a rule.
 */
#ifndef PI_MC_BRIDGE_CONFIGFILE_H
#define PI_MC_BRIDGE_CONFIGFILE_H

#include <stddef.h>

#include "config.h"

/* Parse `path` into `cfg`: [global] keys into the Config defaults and every
 * [trigger:<name>] section appended to cfg->triggers. Existing trigger entries
 * are kept (the table is appended to). Returns 0 on success, -1 on error with a
 * message identifying the offending section/key written to `err`. */
int configfile_load(const char *path, Config *cfg, char *err, size_t errlen);

#endif /* PI_MC_BRIDGE_CONFIGFILE_H */
