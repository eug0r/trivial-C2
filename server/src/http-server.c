#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include "http-server.h"
#include "hash-table.h"
#include "http-parser.h"

#include<openssl/bio.h>
#include<openssl/ssl.h>
#include<openssl/err.h>
#include<openssl/pem.h>
#include<openssl/x509.h>


//only persistent sequential HTTP/1.1 connection is supported. no pipelining or multiplexing
//header keys are stored as lowercase
//need to add support for HEAD request, better understanding and handling of headers, and simple compression

struct client_args {
	int client_fd;
	int (*router_fn)(struct http_response *, struct http_request *);
	SSL_CTX *ctx;
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
//right now it listens on stdin, could do it on a different channel

void handle_signal(int sig) {
	shutdown_flag = 1;
	signal(sig, SIG_IGN); //ignore further signals
}



SSL_CTX *create_server_context(const char *cert_path, const char *key_path) {
	SSL_CTX *ctx;
	ctx = SSL_CTX_new(TLS_server_method());
	if (!ctx) {
		ERR_print_errors_fp(stderr);
		exit(EXIT_FAILURE);
	}
	//set server certificate
	if (SSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM) <= 0) {
		ERR_print_errors_fp(stderr);
		exit(EXIT_FAILURE);
	}
	//set server private key
	if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) <= 0) {
		ERR_print_errors_fp(stderr);
		exit(EXIT_FAILURE);
	}
	//verify
	if (!SSL_CTX_check_private_key(ctx)) {
		fprintf(stderr, "privkey and certificate don't match\n");
		exit(EXIT_FAILURE);
	}
	//SSL_CTX_set_ecdh_auto(ctx, 1);
	//SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
	return ctx;
}


int http_init_server(int (*router)(struct http_response *, struct http_request *)) {

	//setbuf(stdout, NULL);
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

	//ignore sigpipe. when peer sends close notice (fin) but
	//doesn't wait for ours, it causes SSL_shutdown to raise
	//sigpipe. we can catch it with errno == EPIPE
	signal(SIGPIPE, SIG_IGN);

	//initialize ssl
	SSL_load_error_strings();
	SSL_library_init();
	OpenSSL_add_all_algorithms();
	SSL_CTX *ctx = create_server_context(HTTP_SERVER_CERT_PATH, HTTP_SERVER_PRIVKEY_PATH);

	//create worker threads
	pthread_t worker_pool[HTTP_THREAD_POOL_SIZE];
	pthread_mutex_init(&task_queue_mutex, NULL);
	pthread_cond_init(&task_queue_cond, NULL);

	for (int i = 0; i < HTTP_THREAD_POOL_SIZE; i++) {
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

	if (listen(server_fd, HTTP_CONNECTION_BACKLOG) != 0) {
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
		c_args->ctx = ctx;

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

	for (int i = 0; i < HTTP_THREAD_POOL_SIZE; i++) {
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

	SSL_CTX_free(ctx);
	ERR_free_strings();
	EVP_cleanup();

	return 0;
}

void *control_routine(void *args) {
	char buf[16];
	while (fgets(buf, sizeof(buf), stdin)) { //later version listens on a different channel
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
	SSL_CTX *ctx = c_args->ctx;

	struct timeval client_timeout = {
		.tv_sec = 1,
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

	//creating ssl object
	SSL *ssl = SSL_new(ctx);
	if (!ssl) {
		fprintf(stderr, "SSL_new failed\n");
		close(client_fd);
		return -1;
	}
	if (!SSL_set_fd(ssl, client_fd)) {
		fprintf(stderr, "SSL_set_fd failed\n");
		SSL_free(ssl);
		close(client_fd);
		return -1;
	}
	//ssl_accept, handshake
	while (1) {
		int rc = SSL_accept(ssl);
		if (rc == 1)
			break; //handshake complete

		int err = SSL_get_error(ssl, rc);
		if (err == SSL_ERROR_WANT_READ ||
			err == SSL_ERROR_WANT_WRITE ||
			err == SSL_ERROR_WANT_ACCEPT) { //this last one is probably unnecessary
			//timeout triggers this:
			if (shutdown_flag) {
				fprintf(stderr, "shutdown flag caught during SSL_accept WANT_*.\n");
				SSL_shutdown(ssl);
				SSL_free(ssl);
				close(client_fd);
				return -1;
			}
			continue; //retry handshake
		}
		if (err == SSL_ERROR_ZERO_RETURN) {
			fprintf(stderr, "client closed without completing handshake\n");
			SSL_shutdown(ssl); //check errno EPIPE
			SSL_free(ssl);
			close(client_fd);
			return 0;
		}
		if (err == SSL_ERROR_SYSCALL) {
			//probably legacy timeout:
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				fprintf(stderr, "SSL_accept timeout happened\n");
				if (shutdown_flag) {
					fprintf(stderr, "shutdown flag caught during SSL_accept timeout.\n");
					SSL_shutdown(ssl);
					SSL_free(ssl);
					close(client_fd);
					return -1;
				}
				continue; //retry handshake
			}
			fprintf(stderr, "SSL_accept syscall error: %s\n", strerror(errno));
			//SSL_shutdown must not be called
			SSL_free(ssl);
			close(client_fd);
			return 0; //not sure if this could be program-wide fatal. return -1 if so
		}
		if (err == SSL_ERROR_SSL) {
			fprintf(stderr, "SSL_accept protocol error.\n");
			unsigned long e;
			while ((e = ERR_get_error())) {
				char errbuf[256];
				ERR_error_string_n(e, errbuf, sizeof(errbuf));
				fprintf(stderr, "openssl error: %s\n", errbuf);
			}
			//SSL_shutdown should not be called
			SSL_free(ssl);
			close(client_fd);
			return 0;
		}
		//other error
		fprintf(stderr, "SSL_accept failed: rc=%d, err=%d\n", rc, err);
		SSL_shutdown(ssl);
		SSL_free(ssl);
		close(client_fd);
		return 0;
	}
	//handshake success
	//fprintf(stderr, "handshake success\n");

	//main connection loop
	int close_connection = 0;
	while (close_connection == 0 && shutdown_flag == 0) { //here
		struct http_response *http_response = calloc(1, sizeof(struct http_response));
		if (http_response == NULL) {
			fprintf(stderr, "failed to allocate.\n");
			//SSL_shutdown(ssl);
			SSL_free(ssl);
			close(client_fd);
			return -1;
		}
		http_response->stat_line.status_code = 0;
		struct http_request *http_request = http_request_reader(ssl, &http_response->stat_line.status_code, &close_connection);
		if (http_request == NULL) {
			if (http_response->stat_line.status_code == 0) {
				if (close_connection == 1) {
					http_response_free(http_response, HTTP_FREE_STRCT);
					break; //connection closed by the client
				}
				http_response_free(http_response, HTTP_FREE_STRCT);
				//SSL_shutdown(ssl)
				SSL_free(ssl);
				close(client_fd);
				return -1; //failed to allocate before even reading,
				//or shutdown_flag
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
					SSL_free(ssl);
					close(client_fd);
					return -1;
				}
				connection_header->key = "Connection";
				connection_header->value = "close";
				hash_add_node(http_response->headers, connection_header); */
				size_t bytes_sent = 0;
				int rc = http_response_sender(ssl, http_response, 1, &bytes_sent); //1 sets the connection: close header
				if (rc == -1) { //shutdown flag caught or fatal error
					fprintf(stderr, "http_response_sender failed.\n");
					http_response_free(http_response, HTTP_FREE_STATL);
					SSL_free(ssl);
					close(client_fd);
					return -1;
				} //rc == 0 success
				if (bytes_sent == 0) {
					http_response_free(http_response, HTTP_FREE_STATL);
					break;
				}
				printf("%zu bytes were sent.\n", bytes_sent);
				http_response_free(http_response, HTTP_FREE_STATL); //FREE_HDRS if you uncomment the code and pass 0
				break; //closing connection
			}
		}
		else { //http_request is good
			//pass it to the router which in turn passes it to the handlers.
			int result = router_fn(http_response, http_request); //handlers return 0 if they succeed
			if (result == 0) { //success
				size_t bytes_sent = 0;
				int rc = http_response_sender(ssl, http_response, close_connection, &bytes_sent);
				http_request_free(http_request, HTTP_FREE_BODY);
				http_response_free(http_response, HTTP_FREE_BODY);
				if (rc == -1) { //shutdown flag caught or fatal error
					fprintf(stderr, "http_response_sender failed.\n");
					SSL_free(ssl);
					close(client_fd);
					return -1;
				} //rc == 0 success
				if (bytes_sent == 0)
					break;
				printf("%zu bytes were sent.\n", bytes_sent);
				continue;
			}
			else if (result == -1) {
				http_response_free(http_response, HTTP_FREE_BODY);
				http_response = calloc(1, sizeof(struct http_response));
				if (http_response == NULL) {
					fprintf(stderr, "failed to allocate.\n");
					http_request_free(http_request, HTTP_FREE_BODY);
					SSL_free(ssl);
					close(client_fd);
					return -1;
				}
				http_response->stat_line.status_code = 500; //internal server error
				size_t bytes_sent = 0;
				int rc = http_response_sender(ssl, http_response, close_connection, &bytes_sent);
				http_request_free(http_request, HTTP_FREE_BODY);
				http_response_free(http_response, HTTP_FREE_BODY);
				if (rc == -1) {
					fprintf(stderr, "http_response_sender failed.\n");
					SSL_free(ssl);
					close(client_fd);
					return -1;
				}
				if (bytes_sent == 0)
					break;
				printf("%zu bytes were sent.\n", bytes_sent);
				continue;
			}
		}
	}
	SSL_free(ssl);
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