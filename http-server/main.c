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
#include "parser.h"

#define CONNECTION_BACKLOG 20
#define MAX_CLIENT_THREADS 10
//only persistent sequential HTTP/1.1 connection is supported. no pipelining or multiplexing

void *client_routine(void *client_fd_ptr);
int echo_handler(struct http_response *http_response, struct http_request *http_request);
int user_agent_handler(struct http_response *http_response, struct http_request *http_request);


void sigchld_handler() {

}

int main() {
	// Disable output buffering
	setbuf(stdout, NULL);
 	setbuf(stderr, NULL);

	// You can use print statements as follows for debugging, they'll be visible when running tests.
	printf("Logs from your program will appear here!\n");


	int server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd == -1) {
	 	fprintf(stderr,"Socket creation failed: %s...\n", strerror(errno));
	 	return 1;
	}

	// Since the tester restarts your program quite often, setting SO_REUSEADDR
	// ensures that we don't run into 'Address already in use' errors
	int reuse = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
		fprintf(stderr,"SO_REUSEADDR failed: %s \n", strerror(errno));
		return 1;
	}

	struct sockaddr_in serv_addr = { .sin_family = AF_INET ,
									 .sin_port = htons(4221),
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

	pthread_t client_threads[MAX_CLIENT_THREADS];

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

		int *fd_ptr = malloc(sizeof client_fd);
		if (fd_ptr == NULL) {
			perror("malloc");
			close(client_fd);
			continue;
		}
		*fd_ptr = client_fd;
		int ret = pthread_create(&client_th, NULL, client_routine, fd_ptr);
		if (ret) {
			fprintf(stderr, "pthread_create(id[%zu]): %s\n", i, strerror(ret));
			free(fd_ptr);
			return 1; //graceful exiting needed
		}
		printf("thread %zu created.\n", i);
		pthread_detach(client_th);
	}
	//if joining threads do it here

	close(server_fd); //need to listen for commands to break the look and run this

	return 0;
}

void *client_routine(void *client_fd_ptr) {
	int client_fd = *(int *)client_fd_ptr;
	free(client_fd_ptr);

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
			//caller_routing_callback_fn()
			int result = echo_handler(http_response, http_request); //so handlers return 0 if they succeed
			if (result == 0) { //success
				size_t bytes_sent = http_response_sender(client_fd, http_response, close_connection);
				printf("%zu bytes were sent.\n", bytes_sent);
			} //what to do with result != error code?
			http_request_free(http_request, HTTP_FREE_BODY);
			http_response_free(http_response, HTTP_FREE_BODY);
		}
	}
	close(client_fd);
	//malloc copy result
	return NULL; //return (void *)&result //in case we join the threads and collect their returns value
}

int echo_handler(struct http_response *http_response, struct http_request *http_request) {
	if (strcmp(http_request->req_line.method, "POST") == 0 ||
		strcmp(http_request->req_line.method, "GET") == 0) {
		char *ptr = strstr(http_request->req_line.origin, "/echo/");
		if (ptr != NULL && ptr == http_request->req_line.origin) { //must be at the beginning
			ptr += 6; //skip "/echo/"
			//set body to rest of origin
			char *echo_str = strdup(ptr);
			if (echo_str == NULL) {
				return -1; //not sure about the return value.
			}
			http_response->stat_line.status_code = 200;
			http_response->stat_line.reason = NULL;
			http_response->body = echo_str;
			http_response->content_length = strlen(echo_str);
			http_response->headers = hash_init_table();
			struct_header *content_type = calloc(1, sizeof(struct_header));
			if (content_type == NULL) {
				return -1;
			}
			content_type->key = strdup("Content-Type");
			content_type->value = strdup("text/plain");
			hash_add_node(http_response->headers, content_type);
		}
		else { //not a valid echo request, error or pass it on to other handlers
			http_response->stat_line.status_code = 400;
			http_response->stat_line.reason = NULL; //if reason is NULL server will use default in serialization part
			http_response->content_length = 0; //server would add this to headers
		}
		return 0;
	}
	return -1;
}

int user_agent_handler(struct http_response *http_response, struct http_request *http_request) {
	if (http_request->headers == NULL) {
		return -1;
	}
	http_response->stat_line.status_code = 200;
	http_response->stat_line.reason = NULL;

	struct_header *user_agent = hash_lookup_node(http_request->headers, "user-agent"); //all headers are stored in lower-case
	if (user_agent) {
		http_response->content_length = strlen("User-Agent: ") + strlen(user_agent->value);
		http_response->body = malloc(http_response->content_length + 1);
		snprintf(http_response->body, http_response->content_length + 1, "User-Agent: %s", (char *)user_agent->value);

	}
	else {
		http_response->content_length = strlen("No User-Agent found.");
		http_response->body = malloc(http_response->content_length);
		memcpy(http_response->body, "No User-Agent found.", http_response->content_length);
	}
	http_response->headers = hash_init_table();
	struct_header *content_type = calloc(1, sizeof(struct_header));
	if (content_type == NULL) {
		return -1;
	}
	content_type->key = strdup("Content-Type");
	content_type->value = strdup("text/plain");
	hash_add_node(http_response->headers, content_type);
	return 0;
}