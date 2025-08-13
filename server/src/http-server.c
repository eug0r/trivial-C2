#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include "hash-table.h"
#include "http-parser.h"

#define CONNECTION_BACKLOG 20
#define MAX_CLIENT_THREADS 10
#define HTTP_PORT_NO 4221
//only persistent sequential HTTP/1.1 connection is supported. no pipelining or multiplexing
//header keys are stored as lowercase
//need to add thread pool

void *client_routine(void *args);

struct cli_routine_args {
	int client_fd;
	int (*router_fn)(struct http_response *, struct http_request *);
};

int http_init_server(int (*router)(struct http_response *, struct http_request *)) {
	// Disable output buffering
	setbuf(stdout, NULL);
 	setbuf(stderr, NULL);

	int server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd == -1) {
	 	fprintf(stderr,"Socket creation failed: %s...\n", strerror(errno));
	 	return 1;
	}

	int reuse = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
		fprintf(stderr,"SO_REUSEADDR failed: %s \n", strerror(errno));
		return 1;
	}

	struct sockaddr_in serv_addr = { .sin_family = AF_INET ,
									 .sin_port = htons(HTTP_PORT_NO),
									 .sin_addr = { htonl(INADDR_ANY) },
									};

	if (bind(server_fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) != 0) {
		fprintf(stderr,"Bind failed: %s \n", strerror(errno));
		return 1;
	}

	//const int connection_backlog = 5; //making this a macro
	if (listen(server_fd, CONNECTION_BACKLOG) != 0) {
		fprintf(stderr,"Listen failed: %s \n", strerror(errno));
		return 1;
	}

	printf("Waiting for a client to connect...\n");

	//pthread_t client_threads[MAX_CLIENT_THREADS]; // unlimited for now

	for (size_t i = 0;;i++) {
		struct sockaddr_in cli_addr;
		socklen_t cli_len = sizeof(cli_addr);
		int client_fd = accept(server_fd, (struct sockaddr *)&cli_addr, &cli_len);
		if (client_fd == -1) {
			fprintf(stderr, "accept no %zu failed: %s\n", i, strerror(errno));
			continue;
		} //client connected
		char ip_addr_pres[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &cli_addr.sin_addr, ip_addr_pres, sizeof(ip_addr_pres));
		printf("server: got connection from %s\n", ip_addr_pres);
		pthread_t client_th;

		struct cli_routine_args *ptr = malloc(sizeof(struct cli_routine_args));
		if (ptr == NULL) {
			perror("malloc");
			close(client_fd);
			continue;
		}
		ptr->client_fd = client_fd;
		ptr->router_fn = router;
		int ret = pthread_create(&client_th, NULL, client_routine, ptr);
		if (ret) {
			fprintf(stderr, "pthread_create(id[%zu]): %s\n", i, strerror(ret));
			free(ptr);
			return 1; //graceful exiting needed
		}
		printf("thread %zu created.\n", i);
		pthread_detach(client_th);
	}
	//if joining threads do it here

	close(server_fd); //need to listen for commands to break the loop and run this
	//create a back-channel thread on another port
	//that you can connect to and send the termination command or something
	return 0;
}

void *client_routine(void *args) {
	int client_fd = ((struct cli_routine_args*)args)->client_fd;
	int (*router_fn)(struct http_response *, struct http_request *) = ((struct cli_routine_args*)args)->router_fn;
	free(args);

	int close_connection = 0;
	while (!close_connection) {
		struct http_response *http_response = calloc(1, sizeof(struct http_response));
		if (http_response == NULL) {
			fprintf(stderr, "failed to allocate.\n");
			close(client_fd);
			return NULL; //not sure about the return codes
		}
		http_response->stat_line.status_code = 0;
		struct http_request *http_request = http_request_reader(client_fd, &http_response->stat_line.status_code, &close_connection);

		if (http_request == NULL) {
			if (http_response->stat_line.status_code == 0) {
				if (close_connection == 1) {
					break; //connection closed by the client
				}
				close(client_fd);
				return NULL; //failed to allocate before even reading
			}
			else {
				//send() proper http response for the error status code.
				http_response->content_length = 0;
				/* http_response->headers = hash_init_table();
				struct_header *connection_header = calloc(1, sizeof(struct_header));
				if (connection_header  == NULL) {
					fprintf(stderr, "failed to allocate.\n");
					http_response_free(http_response, HTTP_FREE_HDRS);
					close(client_fd);
					return -1;
				}
				connection_header->key = "Connection";
				connection_header->value = "close";
				hash_add_node(http_response->headers, connection_header); */
				ssize_t bytes_sent = http_response_sender(client_fd, http_response, 1); //1 sets the connection: close header
				printf("%zu bytes were sent.\n", bytes_sent);
				http_response_free(http_response, HTTP_FREE_STRCT); //FREE_HDRS if you uncomment the code and pass 0
				break; //closing connection
			}
		}
		else {
			//pass it to the router which in turn passes it to the handlers.
			int result = router_fn(http_response, http_request); //handlers return 0 if they succeed
			if (result == 0) { //success
				size_t bytes_sent = http_response_sender(client_fd, http_response, close_connection);
				printf("%zu bytes were sent.\n", bytes_sent);
			}
			else if (result == -1) {
				http_response_free(http_response, HTTP_FREE_BODY);
				http_response = calloc(1, sizeof(struct http_response));
				if (http_response == NULL) {
					fprintf(stderr, "failed to allocate.\n");
					close(client_fd);
					return NULL; //not sure about the return codes
				}
				http_response->stat_line.status_code = 500; //internal server error
				size_t bytes_sent = http_response_sender(client_fd, http_response, close_connection);
				printf("%zu bytes were sent.\n", bytes_sent);
			}
			http_request_free(http_request, HTTP_FREE_BODY);
			http_response_free(http_response, HTTP_FREE_BODY);
		}
	}
	close(client_fd);
	//malloc copy result
	return NULL; //return (void *)&result //in case we join the threads and collect their returns value
}