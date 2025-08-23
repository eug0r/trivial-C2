
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "http-parser.h"

#define HTTP_CONNECTION_BACKLOG 20
#define HTTP_THREAD_POOL_SIZE 20
#define HTTP_PORT_NO 4221

#define HTTP_SERVER_CERT_PATH "../certs/server-cert.pem"
#define HTTP_SERVER_PRIVKEY_PATH "../certs/server-priv.pem"

int http_init_server(int (*router)(struct http_response *, struct http_request *));

#endif //HTTP_SERVER_H
