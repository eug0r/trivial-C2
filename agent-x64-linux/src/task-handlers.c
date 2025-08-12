#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "task-handlers.h"
#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <wait.h>


extern char *agent_id;
extern struct {
    int mean_delay;
    int jitter;
} beacon_config;

json_t *task_ping(json_t *options) {
    int rc;
    json_t *result;
    if (!result) {
#ifdef DEBUG
        fprintf(stderr, "error: json_object()\n");
#endif
        return NULL;
    }
    result = json_pack("{s:s, s:s}", "type", "text", "data", "pong");
    if (!result) {
#ifdef DEBUG
        fprintf(stderr, "error: json_pack()\n");
#endif
        return NULL;
    }
    return result;
}