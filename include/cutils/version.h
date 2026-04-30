#ifndef CUTILS_VERSION_H
#define CUTILS_VERSION_H

/* --- Version introspection ---
 *
 * Returns the c-utils version string baked in at build time.
 * Format: <semver>_<YYYYMMDD>.<HHMM>.<type>
 *   semver       — value of release_version at build time (e.g. "1.0.0")
 *   YYYYMMDD     — UTC build date
 *   HHMM         — UTC build time
 *   type         — release | debug | asan | coverage
 *
 * Example: "1.0.0_20260430.1428.release"
 *
 * The string is provided as a -D flag from the Makefile; build fails loudly
 * if it is missing, so consumers can rely on the return value being non-NULL
 * and well-formed.
 */
const char *cutils_version(void);

#endif
