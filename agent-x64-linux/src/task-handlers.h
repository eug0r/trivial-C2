#ifndef C2_AGENT_TASK_HANDLERS_H
#define C2_AGENT_TASK_HANDLERS_H

#include "jansson.h"

json_t *task_ping(json_t *options);
json_t *task_cmd(json_t *options);
json_t *task_conf(json_t *options);

#endif //C2_AGENT_TASK_HANDLERS_H