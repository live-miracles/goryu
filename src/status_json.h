#ifndef STATUS_JSON_H
#define STATUS_JSON_H

#include <stdio.h>

#include "stream_state.h"

/* The status API's JSON, written from a snapshot it is handed. It never touches the
 * relay's shared state or libsrt, so the Bunryu contract (which fields are null, and
 * when) is unit-tested with fixtures: see docs/4-reference/1-status-api.md. */

typedef struct status_snapshot {
    long long pid;
    long long started_at_ms;
    long long updated_at_ms;
    const char *last_error; /* "" when there is none */
    const relay_stream_state_t
        *states; /* MAX_ACTIVE_SESSIONS entries; an empty streamid is an unused slot */
} status_snapshot_t;

/* Writes the whole object, compact, followed by a newline. */
void status_json_write(FILE *f, const status_snapshot_t *snap);

#endif
