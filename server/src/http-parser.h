#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stdlib.h>
#include <openssl/ssl.h>
//#include "hash-table.h"

typedef struct ht_node struct_header;

struct status_line { //only 1.1 is supported for now so it will be hardcoded in serialization
    //struct {char high; char low}version;
    unsigned int status_code;
    char *reason;
};

struct http_response {
    struct status_line stat_line;
    struct_header **headers;
    size_t content_length;
    char *body;
    //char *trailers;
};
struct request_line {
    char *method;
    union {
        char *origin; //usual case
        char *absolute; //proxy
        char *authority;
        char *asterisk;
    };
    char *version;
};


/*ht_node will be used as
{char *key; char *value; struct ht_node *next}
but to support multiple set cookies can have something like:
{char *key; struct *{int count; char **value} value; struct ht_node *next}
or we can have method that in general folds headers and adds commas between them
by doing a lookup and concatenating if the header-name already exists, but
it particularly checks for "set-cookie" header name at first and stores them in a separate cookie array.
*/
struct http_request {
    struct request_line req_line;
    struct_header **headers;
    size_t content_length; //mandatory, not accepting Transfer-Encoding
    char *body;
};

enum http_req_rc {
    HTTP_REQ_OK, //pass to handlers
    HTTP_REQ_SEND_ERR, //http_response->status_code is setup to the error, pass to response sender
    HTTP_REQ_CLOSED, //peer closed the connection
    HTTP_REQ_THISCONN_ERR, //error specific to this connection happened
    HTTP_REQ_SD_DETECT, //shutdown signal detected
    HTTP_REQ_FATAL_ERR //fatal error, caller should return -1 to worker thread to trigger shutdown
};

enum http_res_rc {
    HTTP_RES_OK, //successfully sent response
    HTTP_RES_CLOSED, //peer closed
    HTTP_RES_THISCONN_ERR, //error specific to this connection happened
    HTTP_RES_SD_DETECT, //shutdown signal detected
    HTTP_RES_FATAL_ERR //fatal error, caller should return -1 to worker thread to trigger shutdown
};

enum http_req_rc http_request_reader(SSL *ssl, struct http_request *http_request,
                                    unsigned int *status_code, int *close_connection);
enum http_res_rc http_response_sender(SSL *ssl, struct http_response *http_response,
                                      int close_connection, size_t *bytes_sent);

ssize_t parse_request_line(char *buf_start, size_t size, struct request_line *req_line, unsigned int *status_code);
ssize_t parse_headers(char *buf_start, size_t size, struct_header ***headers_ptr, unsigned int *status_code);

void http_request_free(struct http_request *http_request, int mode);
void http_response_free(struct http_response *http_response, int mode);
#define HTTP_FREE_STRCT 1
#define HTTP_FREE_REQL 2
#define HTTP_FREE_STATL 2
#define HTTP_FREE_HDRS 3
#define HTTP_FREE_BODY 4

struct http_status_code_reason {
    unsigned int code;
    const char *reason;
};
#define HTTP_STATUS_COVERED 10
extern struct http_status_code_reason http_status_list[HTTP_STATUS_COVERED];


#endif //HTTP_PARSER_H


/*
100 Continue
101 Switching Protocols
102 Processing
103 Early Hints
105-199 Unassigned
200 OK
201 Created
202 Accepted
203 Non-Authoritative Information
204 No Content
205 Reset Content
206 Partial Content
207 Multi-Status
208 Already Reported
209-225 Unassigned
226 IM Used
300 Multiple Choices
301 Moved Permanently
302 Found
303 See Other
304 Not Modified
305 Use Proxy
306 (Unused)
307 Temporary Redirect
308 Permanent Redirect
400 Bad Request
401 Unauthorized
402 Payment Required
403 Forbidden
404 Not Found
405 Method Not Allowed
406 Not Acceptable
407 Proxy Authentication Required
408 Request Timeout
409 Conflict
410 Gone
411 Length Required
412 Precondition Failed
413 Content Too Large
414 URI Too Long
415 Unsupported Media Type
416 Range Not Satisfiable
417 Expectation Failed
421 Misdirected Request
422 Unprocessable Content
423 Locked
424 Failed Dependency
425 Too Early
426 Upgrade Required
428 Precondition Required
429 Too Many Requests
431 Request Header Fields Too Large
451 Unavailable For Legal Reasons
500 Internal Server Error
501 Not Implemented
502 Bad Gateway
503 Service Unavailable
504 Gateway Timeout
505 HTTP Version Not Supported
506 Variant Also Negotiates
507 Insufficient Storage
508 Loop Detected
509 Unassigned
511 Network Authentication Required
 */