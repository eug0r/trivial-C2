

#ifndef C2_AGENT_CONFIG_H
#define C2_AGENT_CONFIG_H

#define DEBUG 1
#define UUID_STR_SIZE 37 //agent and task IDs, 36 + 0x00
#define AGENT_BUFSIZ 4096 //so far only used in reading execl pipe
//server info
#define SERVER_URL "http://127.0.0.1:4221/"
#define AGENTS_URL SERVER_URL "agents"
#define TASKS_URL SERVER_URL "tasks"
#define RESULTS_URL SERVER_URL "results"

//agent info
#define AGENT_HANDLE "myagent"
#define DEFAULT_DELAY_S 3
#define DEFAULT_DELAY_NS 0
#define DEFAULT_JITTER_S 0
#define DEFAULT_JITTER_NS 0

#define MAX_FAIL_RETRY 5 //retry sending beacons if the server is unreachable

#endif //C2_AGENT_CONFIG_H
