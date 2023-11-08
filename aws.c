/*
 * epoll-based echo server. Uses epoll(7) to multiplex connections.
 *
 * TODO:
 *  - block data receiving when receive buffer is full (use circular buffers)
 *  - do not copy receive buffer into send buffer when send buffer data is
 *      still valid
 *
 * 2011-2017, Operating Systems
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "util.h"
#include "debug.h"
#include "sock_util.h"
#include "w_epoll.h"
#include "http_parser.h"
#include "aws.h"

#include <fcntl.h> //pt open
#include <sys/eventfd.h>
#include <libaio.h> //pentru io_setup
#include <sys/sendfile.h> //pentru sendfile

#define ECHO_LISTEN_PORT		42424


/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

enum connection_state {
	STATE_DATA_RECEIVED,
	STATE_DATA_SENT,
	STATE_CONNECTION_CLOSED
};

/* structure acting as a connection handler */
struct connection {
	int sockfd;
	/* buffers used for receiving messages and then echoing them back */
	char recv_buffer[BUFSIZ];
	size_t recv_len;
	char send_buffer[BUFSIZ];
	size_t send_len;
	enum connection_state state;
    int fd;
    int efd;

	int size;
	char **buffer;
	struct iocb *iocb;
	struct iocb **piocb;
	http_parser request_parser;
};

static char request_path[BUFSIZ];	/* storage for request_path */

/*
 * Callback is invoked by HTTP request parser when parsing request path.
 * Request path is stored in global request_path variable.
 */

static int on_path_cb(http_parser *p, const char *buf, size_t len)
{

	//assert(p == &conn.request_parser);
	memcpy(request_path, buf, len);
	return 0;
}

/* Use mostly null settings except for on_path callback. */
static http_parser_settings settings_on_path = {
	/* on_message_begin */ 0,
	/* on_header_field */ 0,
	/* on_header_value */ 0,
	/* on_path */ on_path_cb,
	/* on_url */ 0,
	/* on_fragment */ 0,
	/* on_query_string */ 0,
	/* on_body */ 0,
	/* on_headers_complete */ 0,
	/* on_message_complete */ 0
};

/*
 * Initialize connection structure on given socket.
 */

static struct connection *connection_create(int sockfd)
{
	struct connection *conn = malloc(sizeof(*conn));

	DIE(conn == NULL, "malloc");

	conn->sockfd = sockfd;
	memset(conn->recv_buffer, 0, BUFSIZ);
	memset(conn->send_buffer, 0, BUFSIZ);

	return conn;
}

/*
 * Copy receive buffer to send buffer (echo).
 */

/*
 * static void connection_copy_buffers(struct connection *conn)
 * {
 *	conn->send_len = conn->recv_len;
 * memcpy(conn->send_buffer, conn->recv_buffer, conn->send_len);
 * }
 * Remove connection handler.
 */

static void connection_remove(struct connection *conn)
{
	// inchidem fisierul daca e != null
	if (conn->fd > 0)
		close(conn->fd);
	close(conn->sockfd);
	conn->state = STATE_CONNECTION_CLOSED;
	free(conn);
}

/*
 * Handle a new connection request on the server socket.
 */

static void handle_new_connection(void)
{
	static int sockfd;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	struct connection *conn;
	int rc;

	/* accept new connection */
	sockfd = accept(listenfd, (SSA *) &addr, &addrlen);
	DIE(sockfd < 0, "accept");

	dlog(LOG_ERR, "Accepted connection from: %s:%d\n",
		inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

	/* instantiate new connection handler */
	conn = connection_create(sockfd);

	/* add socket to epoll */
	rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
	DIE(rc < 0, "w_epoll_add_in");
}

/*
 * Receive message on socket.
 * Store message in recv_buffer in struct connection.
 */

static enum connection_state receive_message(struct connection *conn)
{
	ssize_t bytes_recv = 0;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		sprintf(stderr, "%s", "get_peer_address error");
		goto remove_connection;
	}

	// citire partiala !!!! (vezi test.log)
	while (strstr(conn->recv_buffer, "\r\n\r\n") == NULL)
		bytes_recv += recv(conn->sockfd, conn->recv_buffer + bytes_recv, BUFSIZ, 0);

	if (bytes_recv < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication from: %s\n", abuffer);
		goto remove_connection;
	}
	if (bytes_recv == 0) {		/* connection closed */
		dlog(LOG_INFO, "Connection closed from: %s\n", abuffer);
		goto remove_connection;
	}

	dlog(LOG_DEBUG, "Received message from: %s\n", abuffer);

	printf("--\n%s--\n", conn->recv_buffer);

	conn->recv_len = bytes_recv;
	conn->state = STATE_DATA_RECEIVED;

	return STATE_DATA_RECEIVED;

remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

/*
 * Send message on socket.
 * Store message in send_buffer in struct connection.
 */

static enum connection_state send_message(struct connection *conn)
{
	ssize_t bytes_sent = 0;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}

	//analog bytes_recv
	while (bytes_sent < conn->send_len)
		bytes_sent += send(conn->sockfd, conn->send_buffer + bytes_sent, conn->send_len - bytes_sent, 0);

	if (bytes_sent < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication to %s\n", abuffer);
		goto remove_connection;
	}
	if (bytes_sent == 0) {		/* connection closed */
		dlog(LOG_INFO, "Connection closed to %s\n", abuffer);
		goto remove_connection;
	}

	dlog(LOG_DEBUG, "Sending message to %s\n", abuffer);

	if(conn->fd != -1){
		// folosim fstat pentru a extrage dimensiunea fisierului
		struct stat sb;

		fstat(conn->fd, &sb);
		int buf_fd_size = sb.st_size;

		conn->size = buf_fd_size;
		if(strstr(request_path, "static")){
			int total_sent = 0;

			while(total_sent < buf_fd_size){
				rc = sendfile(conn->sockfd, conn->fd, NULL, buf_fd_size);
				if(rc == -1)
					fprintf(stderr, "%s", "sendfile error");
				else{
					total_sent += rc;
					bytes_sent += rc;
				}
			}
		}

		//dynamic case
		else{
			conn->efd = eventfd(0, 0); // asteptarea de evenimente multiple (multiplexare)
			if (conn->efd < 0)
				fprintf(stderr, "%s", "efd error");
			long n = conn->size / BUFSIZ + (conn->size % BUFSIZ == 0 ? 0 : 1);
			io_context_t ctx = 0;

			//no events = 1; io_context_t *ctxp
			rc = io_setup(1, &ctx);
			if(rc  < 0)
				fprintf(stderr, "%s", "io_setup error");
			conn->iocb  = (struct iocb *)malloc(n * sizeof(struct iocb));
			conn->piocb = (struct iocb **)malloc(n * sizeof(struct iocb *));
			rc = 0;
			int size = conn->size > BUFSIZ ? BUFSIZ : conn->size;

			conn->buffer = calloc(n, sizeof(char *));
			for(int i = 0; i < n; i++)
				conn->buffer[i] = calloc(BUFSIZ, sizeof(char));
			// vezi model eventfd-aio-test
			for(int i = 0; i < n; i++){
				conn->piocb[i] = &(conn->iocb[i]);
				io_prep_pread(&(conn->iocb[i]), conn->fd, conn->buffer[i], size, i * BUFSIZ);
				io_set_eventfd(&(conn->iocb[i]), conn->efd); //modif flag IOCB_FLAG_RESFD
				int r  = io_submit(ctx, 1, &(conn->piocb[i]));

				if (r < 0) {
					fprintf(stderr, "%s", "io_submit error");
					return -1;
				}
				int rc = 0;
				// asteptam pt operatii asincron (io_getevents)
				while(rc < 1){
					struct io_event *events;

					events = (struct io_event *) malloc(1 * sizeof(struct io_event));
					rc = io_getevents(ctx, 1, 1, events, NULL);
					free(events);
				}
				conn->piocb[i] = &(conn->iocb[i]);
				io_prep_pwrite(&(conn->iocb[i]), conn->fd, conn->buffer[i], size, i * BUFSIZ);
				io_set_eventfd(&(conn->iocb[i]), conn->efd); //modif flag IOCB_FLAG_RESFD
				r = io_submit(ctx, 1, &(conn->piocb[i]));
				if (r <= 0) {
					fprintf(stderr, "%s", "io_submit error");
					return -1;
				}
				rc = 0;
				// asteptam pt operatii asincron (io_getevents)
				while(rc < 1){
					struct io_event *events;

					events = (struct io_event *) malloc(1 * sizeof(struct io_event));
					rc = io_getevents(ctx, 1, 1, events, NULL);
					free(events);
				}
			}
			io_destroy(ctx);
			free(conn->iocb);
			free(conn->piocb);
			for(int i = 0; i < n; i++)
				free(conn->buffer[i]);
			free(conn->buffer);
		}
	}
}


	fprintf(stderr, "--\n%s--\n", conn->send_buffer);

	/* all done - remove out notification */
	rc = w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_update_ptr_in");

	conn->state = STATE_DATA_SENT;

	//return STATE_DATA_SENT;

remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

/*
 * Handle a client request on a client connection.
 */

static void handle_client_request(struct connection *conn)
{
	int rc;
	size_t bytes_parsed;
	enum connection_state ret_state;

	ret_state = receive_message(conn);
	if(ret_state == STATE_CONNECTION_CLOSED)
		return;
	//connection_copy_buffers(conn);

	/*
	 * Call http_parser to parse sample_request. Subsequently print request_path
	 * as filled by callback.
	 * Callback is on_path_cb as setup in settings_on_path.
	 * init HTTP_REQUEST parser
	 */

	http_parser_init(&conn->request_parser, HTTP_REQUEST);

	memset(request_path, 0, BUFSIZ);
    //conn->recv_buff = simple_request
	bytes_parsed = http_parser_execute(&conn->request_parser, &settings_on_path, conn->recv_buffer, strlen(conn->recv_buffer));
	fprintf(stderr, "Parsed simple HTTP request (bytes: %lu), path: %s\n", bytes_parsed, request_path);

	/*
	 * Verificam in ce caz suntem. Daca avem in request_path
	 * AWS_DOCUMENT_ROOT/static/ , suntem in cazul static (zero-copying).
	 * Daca avem AWS_DOCUMENT/dynamic/, suntem in cazul dinamic (API async)
	 */

	char *pathname = (char *)malloc(BUFSIZ * sizeof(char));

	memset(pathname, 0, BUFSIZ);
	sprintf(pathname, "%s", request_path + 1);
	fprintf(stderr, "%s", pathname);
	conn->fd = open(pathname, O_RDWR);
	if(conn->fd == -1){
		sprintf(conn->send_buffer, "HTTP/1.1 404 Not Found\r\n\r\n");
		conn->send_len = strlen("HTTP/1.1 404 Not Found\r\n\r\n");
	}else{
		sprintf(conn->send_buffer, "HTTP/1.1 200 OK\r\n\r\n");
		conn->send_len = strlen("HTTP/1.1 200 OK\r\n\r\n");
		}
	/* add socket to epoll for out events */
	rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_add_ptr_out");
}

int main(void)
{
	int rc;

	/* init multiplexing */
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	/* create server socket */
	listenfd = tcp_create_listener(AWS_LISTEN_PORT,
		DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	dlog(LOG_INFO, "Server waiting for connections on port %d\n",
		AWS_LISTEN_PORT);

	/* server main loop */
	while (1) {
		struct epoll_event rev;

		/* wait for events */
		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");

		/*
		 * switch event types; consider
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */

		if (rev.data.fd == listenfd) {
			dlog(LOG_DEBUG, "New connection\n");
			if (rev.events & EPOLLIN)
				handle_new_connection();
		} else {
			if (rev.events & EPOLLIN) {
				dlog(LOG_DEBUG, "New message\n");
				handle_client_request(rev.data.ptr);
			}
			if (rev.events & EPOLLOUT) {
				dlog(LOG_DEBUG, "Ready to send message\n");
				send_message(rev.data.ptr);
			}
		}
	}

	return 0;
}

