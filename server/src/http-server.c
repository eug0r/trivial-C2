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
//need to add support for HEAD request, better understanding and handling of headers, and simple compression

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

void *control_routine(void *args); //a thread that listens for commands like quit
//right now it listens on stdin, could do it on a different port

void handle_signal(int sig) {
	shutdown_flag = 1;
	signal(sig, SIG_IGN); //ignore further signals
}


int http_init_server(int (*router)(struct http_response *, struct http_request *)) {
	//disable output buffering
	setbuf(stdout, NULL);
 	setbuf(stderr, NULL);

	//block sigint and sigterm
	sigset_t blockset;
	sigemptyset(&blockset);
	sigaddset(&blockset, SIGINT);
	sigaddset(&blockset, SIGTERM);
	if (pthread_sigmask(SIG_BLOCK, &blockset, NULL) != 0) {
		perror("pthread_sigmask");
		return 1;
	}

	//register signal handler
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	if (sigaction(SIGINT, &sa, NULL) == -1) {
		perror("sigaction SIGINT");
		return 1;
	}
	if (sigaction(SIGTERM, &sa, NULL) == -1) {
		perror("sigaction SIGTERM");
		return 1;
	}
	//create worker threads
	pthread_t worker_pool[THREAD_POOL_SIZE];
	pthread_mutex_init(&task_queue_mutex, NULL);
	pthread_cond_init(&task_queue_cond, NULL);

	for (int i = 0; i < THREAD_POOL_SIZE; i++) {
		if (pthread_create(&worker_pool[i], NULL, &worker_routine, NULL) != 0) {
			perror("pthread_create");
			return 1;
		}
	}

	//create control thread to handle exit
	pthread_t control_thread;
	if (pthread_create(&control_thread, NULL, &control_routine, NULL) != 0) {
		perror("pthread_create");
		shutdown_flag = 1;
		goto join_workers;
	}


	int server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd == -1) {
	 	perror("socket");
		shutdown_flag = 1;
	 	goto join_all_threads;
	}

	int reuse = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
		perror("setsockopt SO_REUSEADDR");
		close(server_fd);
		shutdown_flag = 1;
		goto join_all_threads;
	}

	struct sockaddr_in serv_addr = {
		.sin_family = AF_INET ,
		.sin_port = htons(HTTP_PORT_NO),
		.sin_addr = { htonl(INADDR_ANY) },
	};

	if (bind(server_fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) != 0) {
		perror("bind");
		close(server_fd);
		shutdown_flag = 1;
		goto join_all_threads;
	}

	//const int connection_backlog = 5; //making this a macro
	if (listen(server_fd, CONNECTION_BACKLOG) != 0) {
		perror("listen");
		close(server_fd);
		shutdown_flag = 1;
		goto join_all_threads;
	}

	printf("Waiting for a client to connect...\n");


	while (shutdown_flag == 0) {
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(server_fd, &rfds);

		struct timespec ps_timeout = {
			.tv_sec = 1,
			.tv_nsec = 0
		};

		sigset_t unblockset;
		sigemptyset(&unblockset);

		int res = pselect(server_fd + 1, &rfds,
			NULL, NULL, &ps_timeout, &unblockset);
		if (res < 0) {
			if (errno == EINTR) {
				//caught sig, handler must have set shutdown_flag; loop will exit if set
				continue;
			}
			perror("pselect");
			break;
		}
		if (res == 0) {
			//timeout
			continue;
		}
		if (!FD_ISSET(server_fd, &rfds)) {
			//shouldn't happen, since only one fd was in the set
			//didn't want to indent the rest
			//of the code in an if FD_ISSET block
			continue;
		}
		//FD_ISSET(server_fd, &rfds):
		struct sockaddr_in cli_addr;
		socklen_t cli_len = sizeof(cli_addr);
		int client_fd = accept(server_fd, (struct sockaddr *)&cli_addr, &cli_len);
		if (client_fd == -1) {
			if (errno == EINTR) {
				fprintf(stderr, "accept got interrupted by signal: %s\n", strerror(errno));
				continue;
			}
			//accept failed for other reason:
			perror("accept failed");
			//we can add a condition to continue on non-fatal errors later
			shutdown_flag = 1; //waking workers so they can exit
			pthread_mutex_lock(&task_queue_mutex);
			pthread_cond_broadcast(&task_queue_cond);
			pthread_mutex_unlock(&task_queue_mutex);
			break;
		} //client connected:

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
			pthread_mutex_unlock(&task_queue_mutex);
			free(c_args);
			close(client_fd);
			//enqueue only fails on malloc so we exit, could keep mutex locked
			//instead of unlocking and relocking
			shutdown_flag = 1;
			pthread_mutex_lock(&task_queue_mutex);
			pthread_cond_broadcast(&task_queue_cond);
			pthread_mutex_unlock(&task_queue_mutex);
			break;
		} //added new tasks
		pthread_cond_signal(&task_queue_cond);
		pthread_mutex_unlock(&task_queue_mutex);
		printf("connection task enqueued\n");
	}

	close(server_fd); //close listening socket before waiting on
	//other worker threads

join_all_threads:
	pthread_cancel(control_thread);
	pthread_join(control_thread, NULL);

join_workers:
	//waking workers to exit, if they haven't already
	pthread_mutex_lock(&task_queue_mutex);
	pthread_cond_broadcast(&task_queue_cond);
	pthread_mutex_unlock(&task_queue_mutex);

	for (int i = 0; i < THREAD_POOL_SIZE; i++) {
		if (pthread_join(worker_pool[i], NULL) != 0) {
			perror("Failed to join the thread");
			return 1;
		}
	}

	//cleanup the queue
	pthread_mutex_lock(&task_queue_mutex);
	struct task_node *curr = head;
	while (curr) {
		struct task_node *next = curr->next;
		if (curr->args) {
			close(curr->args->client_fd);
			free(curr->args);
		}
		free(curr);
		curr = next;
	}
	head = tail = NULL;
	pthread_mutex_unlock(&task_queue_mutex);


	pthread_mutex_destroy(&task_queue_mutex);
	pthread_cond_destroy(&task_queue_cond);

	return 0;
}

void *control_routine(void *args) {
	char buf[16];
	while (fgets(buf, sizeof(buf), stdin)) { //later version listens on a different port
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

void *worker_routine(void *args) {
	while (1) {
		pthread_mutex_lock(&task_queue_mutex);
		struct client_args *c_args = NULL;
		while (shutdown_flag == 0 && ((c_args = dequeue_task()) == NULL)) {
			pthread_cond_wait(&task_queue_cond, &task_queue_mutex);
		} //got task or shutdown was set
		if (shutdown_flag == 1) {
			if (c_args)
				free(c_args);
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

	struct timeval client_timeout = {
		.tv_sec = 2,
		.tv_usec = 0
	};

	if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
		&client_timeout, sizeof(client_timeout)) < 0) {
		perror("setsockopt SO_RCVTIMEO");
		close(client_fd);
		return -1;
	}

	if (setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO,
		&client_timeout, sizeof(client_timeout)) < 0) {
		perror("setsockopt SO_SNDTIMEO");
		close(client_fd);
		return -1;
	}

	int close_connection = 0;
	while (!close_connection && shutdown_flag == 0) { //here
		struct http_response *http_response = calloc(1, sizeof(struct http_response));
		if (http_response == NULL) {
			fprintf(stderr, "failed to allocate.\n");
			close(client_fd);
			return -1;
		}
		http_response->stat_line.status_code = 0;
		struct http_request *http_request = http_request_reader(client_fd, &http_response->stat_line.status_code, &close_connection);
		if (http_request == NULL) {
			if (http_response->stat_line.status_code == 0) {
				if (close_connection == 1) {
					http_response_free(http_response, HTTP_FREE_STRCT);
					break; //connection closed by the client
				}
				http_response_free(http_response, HTTP_FREE_STRCT);
				close(client_fd);
				return -1; //failed to allocate before even reading, or shutdown_flag
				//caught in timeout
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
				if (bytes_sent == -1) {
					fprintf(stderr, "http_response_sender failed.\n");
					http_response_free(http_response, HTTP_FREE_STATL);
					close(client_fd);
					return -1;
				}
				printf("%zu bytes were sent.\n", bytes_sent);
				http_response_free(http_response, HTTP_FREE_STATL); //FREE_HDRS if you uncomment the code and pass 0
				break; //closing connection
			}
		}
		else {
			//pass it to the router which in turn passes it to the handlers.
			int result = router_fn(http_response, http_request); //handlers return 0 if they succeed
			if (result == 0) { //success
				size_t bytes_sent = http_response_sender(client_fd, http_response, close_connection);
				if (bytes_sent == -1) {
					fprintf(stderr, "http_response_sender failed.\n");
					http_request_free(http_request, HTTP_FREE_BODY);
					http_response_free(http_response, HTTP_FREE_BODY);
					close(client_fd);
					return -1;
				}
				printf("%zu bytes were sent.\n", bytes_sent);
			}
			else if (result == -1) {
				http_response_free(http_response, HTTP_FREE_BODY);
				http_response = calloc(1, sizeof(struct http_response));
				if (http_response == NULL) {
					fprintf(stderr, "failed to allocate.\n");
					http_request_free(http_request, HTTP_FREE_BODY);
					close(client_fd);
					return -1;
				}
				http_response->stat_line.status_code = 500; //internal server error
				size_t bytes_sent = http_response_sender(client_fd, http_response, close_connection);
				if (bytes_sent == -1) {
					fprintf(stderr, "http_response_sender failed.\n");
					http_request_free(http_request, HTTP_FREE_BODY);
					http_response_free(http_response, HTTP_FREE_BODY);
					close(client_fd);
					return -1;
				}
				printf("%zu bytes were sent.\n", bytes_sent);
			}
			http_request_free(http_request, HTTP_FREE_BODY);
			http_response_free(http_response, HTTP_FREE_BODY);
		}
	}
	close(client_fd);
	return 0;
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