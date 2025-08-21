

#include "http-parser.h"
#include <ctype.h>
#include <stdio.h>
#include <sys/socket.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include "hash-table.h"

extern volatile sig_atomic_t shutdown_flag; //needs to be checked in all I/O calls

struct http_status_code_reason http_status_list[HTTP_STATUS_COVERED] =
{ //update the HTTP_STATUS_COVERED number when adding a new code
    {.code=100, .reason = "100 Continue"},
    {.code=200, .reason = "200 OK"},
    {.code=404, .reason = "404 Not Found"},
    {.code=400, .reason = "400 Bad Request"},
    {.code=500, .reason = "500 Internal Server Error"},
    {.code=411, .reason = "411 Length Required"},
    {.code=413, .reason = "413 Content Too Large"},
    {.code=505, .reason = "505 HTTP Version Not Supported"},
    {.code=415, .reason = "415 Unsupported Media Type"},
    {.code=405, .reason = "405 Method Not Allowed"},
};

struct http_request *http_request_reader(int client_fd, unsigned int *status_code, int *close_connection) {
    struct http_request *http_request = calloc(1, sizeof(struct http_request));
    // struct http_request *http_request = malloc(sizeof(struct http_request));
    // memset(http_request, 0, sizeof(struct http_request));

    if (http_request == NULL) {
        fprintf(stderr, "failed to allocate.\n");
        return NULL; //if status_code not set and returned NULL, request isn't even read.
    }
    char buf[BUFSIZ];
    ssize_t bytes_total = 0, bytes_recvd;

    int is_first_iter = 1;
    while (true) {
        bytes_recvd = recv(client_fd, buf+bytes_total, BUFSIZ-bytes_total, 0);

        if (bytes_recvd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                //timeout
                if (shutdown_flag) {
                    fprintf(stderr, "shutdown flag caught during recv timeout\n");
                    http_request_free(http_request, HTTP_FREE_STRCT);
                    return NULL;
                }
                //no shutdown
                continue;
            }
            //other errors
            fprintf(stderr,"error reading from socket: %s\n", strerror(errno));
            *status_code = 500; //internal server error
            http_request_free(http_request, HTTP_FREE_STRCT);
            return NULL;
        }
        if (bytes_recvd == 0) {
            if (is_first_iter) {
                *close_connection = 1;
                http_request_free(http_request, HTTP_FREE_STRCT);
                return NULL;
            }
            fprintf(stderr, "client didn't complete request.\n");
            *status_code = 400;
            http_request_free(http_request, HTTP_FREE_STRCT);
            return NULL;
        }
        bytes_total+=bytes_recvd;

        ssize_t bytes_parsed = parse_request_line(buf, bytes_total, &http_request->req_line, status_code);
        if (bytes_parsed == -1) {
            fprintf(stderr, "invalid request line.\n");
            http_request_free(http_request, HTTP_FREE_STRCT);
            return NULL; //status code is already set up.
        }
        if (bytes_parsed == 0 && bytes_total >= BUFSIZ) {
            fprintf(stderr, "request line too large.\n");
            *status_code = 400; //bad request
            http_request_free(http_request, HTTP_FREE_STRCT);
            return NULL;
        }
        if (bytes_parsed) { //found request line, breaking
            bytes_total = bytes_total - bytes_parsed;
            memmove(buf, &buf[bytes_parsed], bytes_total);
            break; //goto headers
        }

        is_first_iter = 0;
    }
    // do request line processing here.

    while (true) {
        ssize_t bytes_parsed = parse_headers(buf, bytes_total, &http_request->headers, status_code);
        if (bytes_parsed == -1) {
            fprintf(stderr, "invalid header section.\n");
            //*status_code = 400;
            http_request_free(http_request, HTTP_FREE_REQL);
            return NULL; //status code is already set up.
        }
        if (bytes_parsed == 0 && bytes_total >= BUFSIZ) {
            fprintf(stderr, "headers too large.\n");
            *status_code = 400;
            http_request_free(http_request, HTTP_FREE_REQL);
            return NULL;
        }
        if (bytes_parsed) { //found headers, breaking
            bytes_total = bytes_total - bytes_parsed;
            memmove(buf, &buf[bytes_parsed], bytes_total);
            break; //goto body
        }
        //no headers section yet (\r\n\r\n) waiting for more bytes:
continue_recv: //for recovering from timeout
        bytes_recvd = recv(client_fd, buf+bytes_total, BUFSIZ-bytes_total, 0);
        if (bytes_recvd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                //timeout
                if (shutdown_flag) {
                    fprintf(stderr, "shutdown flag caught during recv timeout\n");
                    http_request_free(http_request, HTTP_FREE_REQL);
                    return NULL;
                }
                //no shutdown
                goto continue_recv;
            }
            fprintf(stderr,"error reading from socket: %s\n", strerror(errno));
            *status_code = 500;
            http_request_free(http_request, HTTP_FREE_REQL);
            return NULL;
        }
        //is the connection closed?
        if (bytes_recvd == 0) {
            fprintf(stderr, "client didn't complete request.\n");
            *status_code = 400;
            http_request_free(http_request, HTTP_FREE_REQL);
            return NULL;
        }
        bytes_total+=bytes_recvd;
    }

    //do headers processing here. check Content-length if not GET or other body-less methods
    struct_header *connection = hash_lookup_node(http_request->headers, "connection");
    if (connection == NULL) {
        *close_connection = 0; //persistent by default
    } else if (strcmp(connection->value, "close") == 0) {
        *close_connection = 1; //closing the tcp connection
    } else if (strcmp(connection->value, "keep-alive") == 0) {
        *close_connection = 0; //add keep-alive header processing and
        //negotiation here if needed in the future
    } else { //bogus value
        fprintf(stderr, "invalid connection header.\n");
        *status_code = 400;
        http_request_free(http_request, HTTP_FREE_HDRS);
        return NULL;
    }

    if (strcmp(http_request->req_line.method, "GET") == 0)
        return http_request; //no body to parse

    struct_header *content_length = hash_lookup_node(http_request->headers, "content-length");
    if (content_length == NULL ) { //must have content length header
        fprintf(stderr, "no content-length.\n");
        *status_code = 400;
        http_request_free(http_request, HTTP_FREE_HDRS);
        return NULL;
    }
    char *endcptr;
    long long length = strtoll(content_length->value, &endcptr, 10);
    if (content_length->value == endcptr || *endcptr != '\0') {
        fprintf(stderr, "invalid content-length.\n");
        *status_code = 400;
        http_request_free(http_request, HTTP_FREE_HDRS);
        return NULL; //more robust to errno=0 pre-call then check errno for overflow
        //and add max_content_length
    } //length is valid, reading body:

    char *body_buf = malloc(length); //free this
    if (!body_buf) {
        fprintf(stderr, "malloc failed: %s\n", strerror(errno));
        *status_code = 500;
        http_request_free(http_request, HTTP_FREE_HDRS);
        return NULL;
    }
    if (length <= bytes_total) {
        memcpy(body_buf, buf, length);
        http_request->body = body_buf;
    }
    else { //full body isn't received yet
        memcpy(body_buf, buf, bytes_total);
        while (true) {
            bytes_recvd = recv(client_fd, body_buf+bytes_total, length-bytes_total, 0);
            if (bytes_recvd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    //timeout
                    if (shutdown_flag) {
                        fprintf(stderr, "shutdown flag caught during recv timeout\n");
                        free(body_buf);
                        http_request_free(http_request, HTTP_FREE_HDRS);
                        return NULL;
                    }
                    //no shutdown
                    continue;
                }
                fprintf(stderr,"error reading from socket: %s\n", strerror(errno));
                *status_code = 500;
                free(body_buf);
                http_request_free(http_request, HTTP_FREE_HDRS);
                return NULL;
            }
            bytes_total+=bytes_recvd;
            if (bytes_total >= length) {
                http_request->body = body_buf;
                break; //success!!
            }
            if (bytes_recvd == 0) {
                fprintf(stderr, "client didn't complete request.\n");
                *status_code = 400;
                free(body_buf);
                http_request_free(http_request, HTTP_FREE_HDRS);
                return NULL;
            }
        }
    }

    http_request->content_length = length;
    return http_request;
}
ssize_t parse_request_line(char *buf_start, size_t size, struct request_line *req_line, unsigned int *status_code) {
    for (ssize_t i = 0; i < size-1; i++) {
        if (buf_start[i] == '\r' && buf_start[i+1] == '\n') { //found
            char req_line_buf[i+1];
            memcpy(req_line_buf, buf_start, i);
            req_line_buf[i] = '\0'; //could check if any nullbytes were in the buffer and reject it
            if (strlen(req_line_buf) != i) {
                *status_code = 400;
                return -1; //bad request, null bytes inside it.

            }
            char method_buf[128];
            char path_buf[BUFSIZ];
            char version_buf[128];
            sscanf(req_line_buf, "%127[A-Z] %[^ ] HTTP/%127[0-9.]",
                method_buf, path_buf, version_buf);
            //is_valid_method here
            req_line->method = strdup(method_buf);
            if (req_line->method == NULL) {
                fprintf(stderr,"failed to allocate\n");
                *status_code = 500;
                return -1; //no resource to free
            }
            //is_valid_path here
            req_line->origin = strdup(path_buf);
            if (req_line->origin == NULL) {
                fprintf(stderr,"failed to allocate\n");
                *status_code = 500;
                free(req_line->method);
                return -1;
            }
            if (strncmp(version_buf, "1.1", 3) != 0) {
                *status_code = 505;
                free(req_line->method);
                free(req_line->origin);
                return -1; //version unsupported
            }
            req_line->version = strdup(version_buf);
            if (req_line->version == NULL) {
                fprintf(stderr,"failed to allocate\n");
                *status_code = 500;
                free(req_line->method);
                free(req_line->origin);
                return -1;
            }
            return i+2; //skip the CRLF
        }
    }
    return 0;
}
ssize_t parse_headers(char *buf_start, size_t size, struct_header ***headers_ptr, unsigned int *status_code) {
    char *pos = buf_start; //buf_start remains the base
    if (size == 2 && buf_start[0] == '\r' && buf_start [1] == '\n') { //empty headers
        *headers_ptr = hash_init_table();
        if (*headers_ptr == NULL) {
            fprintf(stderr,"failed to allocate\n");
            *status_code = 500;
            return -1;
        }
        return 2; //parsed two bytes
    }
    for (ssize_t i = 0; i < size - 3; i++) {
        if (buf_start[i] == '\r' &&
            buf_start[i+1] == '\n' &&
            buf_start[i+2] == '\r' &&
            buf_start[i+3] == '\n') {
            //headers found
            *headers_ptr = hash_init_table();
            if (*headers_ptr == NULL) {
                fprintf(stderr,"failed to allocate\n");
                *status_code = 500;
                return -1;
            }
            struct_header **headers = *headers_ptr;
            while (true) {
                struct_header *header = calloc(1, sizeof(struct_header));
                if (header == NULL) {
                    fprintf(stderr,"failed to allocate\n");
                    *status_code = 500;
                    hash_free_table(headers, free);
                    return -1;
                }
                char key_buf[BUFSIZ];
                char val_buf[BUFSIZ];
                if (sscanf(pos, "%[^:\r\n]: %[^\r\n]", key_buf, val_buf) != 2) {
                    //malformed header
                    //free previous ones
                    *status_code = 400;
                    free(header);
                    hash_free_table(headers, free);
                    return -1;
                }
                for (unsigned char *p = key_buf; *p; p++) *p = tolower(*p); //lowercase the string, assuming asccii
                header->key = strdup(key_buf);
                if (header->key == NULL) {
                    fprintf(stderr,"failed to allocate\n");
                    *status_code = 500;
                    free(header);
                    hash_free_table(headers, free);
                    return -1;
                }
                header->value = strdup(val_buf);
                if (header->value == NULL) {
                    fprintf(stderr,"failed to allocate\n");
                    *status_code = 500;
                    free(header->key);
                    free(header);
                    hash_free_table(headers, free);
                    return -1;
                }
                //for header merging do -> check if key is Set-Cookie ->
                //if it is, add to cookies (not impl yet) if not, check if
                //key already exists (hash_lookup_node(key)). concat new value
                //to old one, comma separated. then replace the value.
                hash_add_node(headers, header);
                pos = strstr(pos, "\r\n");
                if (pos == NULL) {
                    fprintf(stderr,"strstr failed\n");
                    *status_code = 500;
                    hash_free_table(headers, free); //current header is already in the table
                    return -1;
                }
                if (pos == &buf_start[i])
                    break; //end of buffer
                pos +=2; //skip a CRLF
            }
            return i+4; //skip two CRLFs
        }
    }
    return 0;
}

void http_request_free(struct http_request *http_request, int mode) {
    //define macros for mode
    if (http_request == NULL)
        return;
    switch (mode) {
        case HTTP_FREE_BODY: {
            if (http_request->body)
                free(http_request->body);
        }
        case HTTP_FREE_HDRS: {
            if (http_request->headers)
                hash_free_table(http_request->headers, free); //pass stdlib free fn
        }
        case HTTP_FREE_REQL: {
            if (http_request->req_line.method)
                free(http_request->req_line.method);
            if (http_request->req_line.origin)
                free(http_request->req_line.origin);
            if (http_request->req_line.version)
            free(http_request->req_line.version);
            //req-line isn't a pointer, will be freed in next case
        }
        case HTTP_FREE_STRCT: {
            free(http_request);
            break;
        }
        default: {
            free(http_request);
        }
    }

}

void http_response_free(struct http_response *http_response, int mode) {
    //define macros for mode
    if (http_response == NULL)
        return;
    switch (mode) {
        case HTTP_FREE_BODY: {
            if (http_response->body)
                free(http_response->body);
        }
        case HTTP_FREE_HDRS: {
            if (http_response->headers)
                hash_free_table(http_response->headers, free); //pass stdlib free fn
        }
        case HTTP_FREE_STATL: {
            if (http_response->stat_line.reason)
                free(http_response->stat_line.reason);
        }
        case HTTP_FREE_STRCT: {
            free(http_response);
            break;
        }
        default: {
            free(http_response);
        }
    }

}


ssize_t send_all(int sockfd, const void *buf, size_t len, int flags); //helper for send

ssize_t http_response_sender(int client_fd, struct http_response *http_response, int close_connection) {
    //initialize the struct to zero before filling with data and passing here
    size_t bufsiz = BUFSIZ;
    char *msg_buf = malloc(BUFSIZ); //free at the end, return number of bytes sent
    if (msg_buf == NULL)
        return -1;

    size_t cur_len = 0;
    size_t msg_len = 0;

    const char *version_str = "HTTP/1.1 "; //leave the SP here
    // adding it to the msg
    cur_len = strlen(version_str);
    #define HTTP_RESPONSE_BUF_REALLOC() do{ \
        if (bufsiz <= msg_len + cur_len) { \
            while(bufsiz <= msg_len + cur_len) \
                bufsiz *= 2; \
            char *temp_ptr = realloc(msg_buf, bufsiz); \
            if (temp_ptr == NULL) { \
                free(msg_buf); \
                return -1; \
            } \
            msg_buf = temp_ptr; \
        } \
    } while(0) //wrap this boilerplate up, I don't know if this is bad practice
    HTTP_RESPONSE_BUF_REALLOC();
    snprintf(msg_buf + msg_len, bufsiz - msg_len, "%s", version_str);
    msg_len += cur_len; //done adding

    if (http_response->stat_line.reason == NULL) { //reason is left empty by the backend handlers
        for (int i = 0; i < HTTP_STATUS_COVERED; i++) {
            if (http_response->stat_line.status_code == http_status_list[i].code) {
                http_response->stat_line.reason = strdup(http_status_list[i].reason);
                if (http_response->stat_line.reason == NULL) { //strdup failure
                    free(msg_buf);
                    return -1;
                }
                break;
            }
        }
        if (http_response->stat_line.reason == NULL) { //couldn't find the status code entry
            free(msg_buf);
            return -1;
        }
    }
    cur_len = strlen(http_response->stat_line.reason) + 2; //CRLF
    HTTP_RESPONSE_BUF_REALLOC();
    snprintf(msg_buf + msg_len, bufsiz - msg_len, "%s\r\n", http_response->stat_line.reason);
    msg_len += cur_len;

    if (http_response->headers != NULL) {
        for (size_t i = 0; i < HASH_TABLE_SIZE; i++) {
            struct ht_node *curr = http_response->headers[i];
            while (curr) {
                if (strcasecmp(curr->key, "Content-Length") == 0)
                    continue; //content-length header will be added from the struct element.

                if (curr->value) { //skip headers with no value
                    cur_len = strlen(curr->key) + 2 + strlen((char *)curr->value) + 2; //: SP CRLF
                    HTTP_RESPONSE_BUF_REALLOC();
                    snprintf(msg_buf + msg_len, bufsiz - msg_len, "%s: %s\r\n",curr->key, (char *)curr->value);
                    msg_len += cur_len;
                }
                curr = curr->next;
            }
        }
    }
    if (close_connection) {
        cur_len = strlen("Connection: close") + 2; //CRLF
        HTTP_RESPONSE_BUF_REALLOC();
        snprintf(msg_buf + msg_len, bufsiz - msg_len, "Connection: close\r\n");
        msg_len += cur_len;
    }
    cur_len = strlen("Content-Length: ") + snprintf(NULL, 0, "%zu", http_response->content_length) + 4; //\r\n\r\n
    HTTP_RESPONSE_BUF_REALLOC();
    snprintf(msg_buf + msg_len, bufsiz - msg_len, "Content-Length: %zu\r\n\r\n", http_response->content_length);
    msg_len += cur_len;


    if (http_response->content_length != 0) {
        cur_len  = http_response->content_length;
        HTTP_RESPONSE_BUF_REALLOC();
        memcpy(msg_buf + msg_len, http_response->body, http_response->content_length);
        msg_len += cur_len; //this never included the trailing null-term and neither should the memcpy
    }

    ssize_t bytes_sent = send_all(client_fd, msg_buf, msg_len, 0);
    free(msg_buf); //response struct will be free'd by the caller
    return bytes_sent;
}

ssize_t send_all(int sockfd, const void *buf, size_t len, int flags) {
    ssize_t total_sent = 0;
    const char *p = buf;
    while (total_sent < len) {
        ssize_t n = send(sockfd, p + total_sent, len - total_sent, flags);
        if (n < 0) {
            if (errno == EINTR) {
                if (shutdown_flag) {
                    fprintf(stderr, "shutdown flag caught after send got interrupted\n");
                    return -1;
                }
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                //timeout
                if (shutdown_flag) {
                    fprintf(stderr, "shutdown flag caught during send timeout\n");
                    return -1;
                }
                continue;
            }
            fprintf(stderr, "error sending: %s\n", strerror(errno));
            return -1;
        }
        if (n == 0) {
            //socket closed unexpectedly
            fprintf(stderr, "send returned 0 (client closed connection?)\n");
            return total_sent;
        }
        //positive n:
        total_sent += n;
    }
    return total_sent;
}