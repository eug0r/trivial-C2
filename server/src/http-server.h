
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

int http_init_server(int (*router)(struct http_response *, struct http_request *));

#endif //HTTP_SERVER_H
