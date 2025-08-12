

#ifndef C2_AGENT_CONFIG_H
#define C2_AGENT_CONFIG_H

#define DEBUG 1
#define UUID_STR_SIZE 37 //agent and task IDs, 36 + 0x00

//server info
#define SERVER_URL "http://127.0.0.1:4221/"
#define AGENTS_URL SERVER_URL "agents"
#define TASKS_URL SERVER_URL "tasks"
#define RESULTS_URL SERVER_URL "results"

//agent info
#define AGENT_HANDLE "myagent"

#define MAX_FAIL_RETRY 5

#endif //C2_AGENT_CONFIG_H