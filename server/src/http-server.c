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
#include <signal.h>
#include "hash-table.h"
#include "http-parser.h"

#define CONNECTION_BACKLOG 20
#define THREAD_POOL_SIZE 20
#define HTTP_PORT_NO 4221
//only persistent sequential HTTP/1.1 connection is supported. no pipelining or multiplexing
//header keys are stored as lowercase
//need to add thread pool

struct client_args {
	int client_fd;
	int (*router_fn)(struct http_response *, struct http_request *);
};
struct task_node {
	struct client_args *args;
	struct task_node *next;
};

pthread_mutex_t task_queue_mutex;
pthread_cond_t task_queue_cond;

volatile sig_atomic_t shutdown_flag = 0;

struct task_node *head = NULL;
struct task_node *tail = NULL;
int enqueue_task(struct client_args *args);
struct client_args *dequeue_task(void);
void *worker_routine(void *args);
int handle_client(struct client_args *c_args);


void *control_routine(void *args) {
	char buf[16];
	while (fgets(buf, sizeof(buf), stdin)) { //later version listen on a different port
		if (strncmp(buf, "quit", 4) == 0) {
			shutdown_flag = 1;
			pthread_mutex_lock(&task_queue_mutex);
			pthread_cond_broadcast(&task_queue_cond);
			pthread_mutex_unlock(&task_queue_mutex);
			break;
		}
	}
	return NULL;
}

void handle_signal(int sig) {
	shutdown_flag = 1;
	signal(sig, SIG_IGN); //ignore further signals
}

int http_init_server(int (*router)(struct http_response *, struct http_request *)) {
	//disable output buffering
	setbuf(stdout, NULL);
 	setbuf(stderr, NULL);

	//register signal handlers
	struct sigaction sa = {0};
	sa.sa_handler = handle_signal;
	sigemptyset(&sa.sa_mask); //check this
	sa.sa_flags = 0; //we want accept to break with EINTR
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	//create worker threads
	pthread_t worker_pool[THREAD_POOL_SIZE];
	pthread_mutex_init(&task_queue_mutex, NULL);
	pthread_cond_init(&task_queue_cond, NULL);

	for (int i = 0; i < THREAD_POOL_SIZE; i++) {
		if (pthread_create(&worker_pool[i], NULL, &worker_routine, NULL) != 0) {
			perror("Failed to create thread");
			return 1;
		}
	}

	//create control thread to handle exit
	pthread_t control_thread;
	if (pthread_create(&control_thread, NULL, &control_routine, NULL) != 0) {
		perror("Failed to create thread");
		return 1;
	}


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


	while (shutdown_flag == 0) {
		struct sockaddr_in cli_addr;
		socklen_t cli_len = sizeof(cli_addr);
		int client_fd = accept(server_fd, (struct sockaddr *)&cli_addr, &cli_len);
		if (client_fd == -1) {
			if (shutdown_flag) {
				fprintf(stderr, "accept got interrupted by signal: %s\n", strerror(errno));
				pthread_mutex_lock(&task_queue_mutex);
				pthread_cond_broadcast(&task_queue_cond);
				pthread_mutex_unlock(&task_queue_mutex);
				break;
			} //accept got interrupted by signal
			//accept failed normally:
			fprintf(stderr, "accept failed: %s\n", strerror(errno));
			continue; //break
		} //client connected
		char ip_addr_pres[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &cli_addr.sin_addr, ip_addr_pres, sizeof(ip_addr_pres));
		printf("server: got connection from %s\n", ip_addr_pres);

		struct client_args *c_args = malloc(sizeof(struct client_args));
		if (c_args == NULL) {
			perror("malloc");
			close(client_fd);
			shutdown_flag = 1;
			pthread_mutex_lock(&task_queue_mutex);
			pthread_cond_broadcast(&task_queue_cond);
			pthread_mutex_unlock(&task_queue_mutex);
			break;
		}
		c_args->client_fd = client_fd;
		c_args->router_fn = router;

		pthread_mutex_lock(&task_queue_mutex);
		if (enqueue_task(c_args) == -1) {
			shutdown_flag = 1;
			pthread_cond_broadcast(&task_queue_cond);
			pthread_mutex_unlock(&task_queue_mutex);
			free(c_args);
			close(client_fd);
			break;
		} //added new tasks
		pthread_cond_signal(&task_queue_cond);
		pthread_mutex_unlock(&task_queue_mutex);
		printf("connection task enqueued\n");
	}

	pthread_cancel(control_thread);
	pthread_join(control_thread, NULL);

	for (int i = 0; i < THREAD_POOL_SIZE; i++) {
		if (pthread_join(worker_pool[i], NULL) != 0) {
			perror("Failed to join the thread");
			return 1;
		}
	}

	pthread_mutex_destroy(&task_queue_mutex);
	pthread_cond_destroy(&task_queue_cond);
	close(server_fd);
	return 0;
}

void *worker_routine(void *args) {
	while (1) {
		pthread_mutex_lock(&task_queue_mutex);
		struct client_args *c_args = NULL;
		while (shutdown_flag == 0 && ((c_args = dequeue_task()) == NULL)) {
			pthread_cond_wait(&task_queue_cond, &task_queue_mutex);
		} //got task or shutdown was set
		if (shutdown_flag == 1) {
			pthread_mutex_unlock(&task_queue_mutex);
			break;  //exit
		}
		pthread_mutex_unlock(&task_queue_mutex);
		int rc = handle_client(c_args); //handle_client frees c_args
		free(c_args);
		if (rc == -1) { //hardcore error
			shutdown_flag = 1;
			pthread_cond_broadcast(&task_queue_cond);
			break;
		}
	}
	return NULL;
}

int handle_client(struct client_args *c_args) {
	if (!c_args)
		return -1;
	int client_fd = c_args->client_fd;
	int (*router_fn)(struct http_response *, struct http_request *) = c_args->router_fn;

	int close_connection = 0;
	while (!close_connection) {
		struct http_response *http_response = calloc(1, sizeof(struct http_response));
		if (http_response == NULL) {
			fprintf(stderr, "failed to allocate.\n");
			close(client_fd);
			return -1; //not sure about the return codes
		}
		http_response->stat_line.status_code = 0;
		struct http_request *http_request = http_request_reader(client_fd, &http_response->stat_line.status_code, &close_connection);
		if (http_request == NULL) {
			if (http_response->stat_line.status_code == 0) {
				if (close_connection == 1) {
					break; //connection closed by the client
				}
				close(client_fd);
				return -1; //failed to allocate before even reading
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
					return -1; //not sure about the return codes
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
	return 0; // connection was terminated "safely" though the handlers don't distinguish between
	// malloc error or less important ones, and return -1 in either case, so those won't be caught
}


int enqueue_task(struct client_args *args) {
	struct task_node *new = malloc(sizeof(struct task_node));
	if (new == NULL) {
		perror("malloc");
		return -1;
	}
	new->args = args;
	new->next = (void *)NULL;
	if (tail == NULL)
		head = new;
	else
		(tail)->next = new;
	tail = new;
	return 0;
}
struct client_args *dequeue_task(void) {
	if (head == NULL)
		return (void *)NULL;
	else {
		struct client_args *args = head->args;
		struct task_node *temp = head;
		head = head->next;
		if (head == NULL)
			tail = (void *)NULL;
		free(temp);
		return args;
	}
}