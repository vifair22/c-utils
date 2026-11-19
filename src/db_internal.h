#ifndef CUTILS_DB_INTERNAL_H
#define CUTILS_DB_INTERNAL_H

#include "cutils/db.h"

/* Library-internal db helpers, not part of the public API. */

/* Nonzero while the connection has a transaction open. SQLite can end
 * one on its own when a statement fails (temp-store failure,
 * SQLITE_FULL, SQLITE_IOERR, RAISE(ROLLBACK)), so recovery code checks
 * this before issuing ROLLBACK or ROLLBACK TO. */
int db_in_transaction(cutils_db_t *db);

/* SQLite's extended result code for the most recent failed call on the
 * connection. Read it straight after the failure, before any other
 * statement runs on the handle. */
int db_last_errcode(cutils_db_t *db);

#endif
