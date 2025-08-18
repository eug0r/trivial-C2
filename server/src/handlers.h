
#ifndef HANDLERS_H
#define HANDLERS_H

#include "http-parser.h"
#define C2_DB_PATH "database.db"
#define BUSY_TIMEOUT 10000
int router(struct http_response *http_response, struct http_request *http_request);

int no_content_ok(struct http_response *http_response);
int method_not_allowed(struct http_response *http_response);
int bad_request(struct http_response *http_response);
int not_found(struct http_response *http_response);

int post_agents(struct http_response *http_response, struct http_request *http_request);
int get_agents(struct http_response *http_response, struct http_request *http_request);
int post_tasks(struct http_response *http_response, struct http_request *http_request);
int get_tasks(struct http_response *http_response, struct http_request *http_request);
int post_results(struct http_response *http_response, struct http_request *http_request);
int get_results(struct http_response *http_response, struct http_request *http_request);
#endif //HANDLERS_H
