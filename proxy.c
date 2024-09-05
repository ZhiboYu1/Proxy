/*
 * Web Proxy
 *
 * This program implements a multithreaded HTTP proxy.
 *
 * Michael Yu
 */

#include <assert.h>
#include <unistd.h>

#include "csapp.h"

static void client_error(int fd, const char *cause, int err_num,
    const char *short_msg, const char *long_msg);
static char *create_log_entry(const struct sockaddr_in *sockaddr,
    const char *uri, int size);
static int parse_uri(const char *uri, char **hostnamep, char **portp,
    char **pathnamep);

// Initialization
#define SBUFSIZE 200
#define NTHREADS 20
pthread_mutex_t log_mutex;
pthread_mutex_t threadlock;
FILE *log_file;

/* sbuf_t: Bounded buffer used by the Sbuf package(adapated version). */
typedef struct {
	int *buf;		 // Buffer array
	int n;			 // Maximum number of slots
	int front;		 // buf[(front+1)%n] is first item
	int rear;		 // buf[rear%n] is last item
	pthread_mutex_t mutex;	 // Protects accesses to buf
	pthread_cond_t notEmpty; // Wait for buf to be not empty
	pthread_cond_t notFull;	 // Wait for buf to be not full
} sbuf_t;

// New functions
void *doit(int connfd);
void sbuf_init(sbuf_t *sp, int n);
void sbuf_deinit(sbuf_t *sp);
void sbuf_insert(sbuf_t *sp, int item);
int sbuf_remove(sbuf_t *sp);

/*
 * Requires:
 * 	sp must point to a valid sbuf_t instance allocated by the caller.
 * 	n must be greater than zero, indicating the buffer's capacity.
 *
 * Effects:
 * 	Initializes the buffer sp with capacity n.
 * 	Allocates memory for the buffer array.
 * 	Sets front and rear to 0, indicating an empty buffer.
 * 	Initializes the mutex and condition variables used for managing access
 * and synchronization.
 */
void
sbuf_init(sbuf_t *sp, int n)
{
	sp->buf = Calloc(n, sizeof(int));
	sp->n = n;
	sp->front = sp->rear = 0;
	pthread_mutex_init(&sp->mutex, NULL);
	pthread_cond_init(&sp->notEmpty, NULL);
	pthread_cond_init(&sp->notFull, NULL);
}

/*
 * Requires:
 * 	sp must point to an initialized sbuf_t instance.
 *
 * Effects:
 * 	Frees the buffer array allocated during initialization.
 * 	Destroys the mutex and condition variables.
 * 	Leaves the buffer in a state where it cannot be used without
 * re-initialization.
 */

void
sbuf_deinit(sbuf_t *sp)
{
	Free(sp->buf);
	pthread_mutex_destroy(&sp->mutex);
	pthread_cond_destroy(&sp->notEmpty);
	pthread_cond_destroy(&sp->notFull);
}

/**
 * Requires:
 * 	sp must point to an initialized sbuf_t instance.
 * 	The buffer must not be full at the time of call (handled internally).
 *
 * Effects:
 * 	Inserts item into the buffer at the position indicated by rear.
 * 	Blocks if the buffer is full until space becomes available.
 * 	Signals potentially waiting consumers via the notEmpty condition
 * variable once an item is added. Ensures that access to the buffer is
 * thread-safe using mutexes.
 */
void
sbuf_insert(sbuf_t *sp, int item)
{
	pthread_mutex_lock(&sp->mutex);
	while ((sp->rear + 1) % sp->n == sp->front) { // Buffer is full
		pthread_cond_wait(&sp->notFull, &sp->mutex);
	}
	sp->rear = (sp->rear + 1) % sp->n;
	sp->buf[sp->rear] = item;
	pthread_cond_signal(&sp->notEmpty);
	pthread_mutex_unlock(&sp->mutex);
}

/**
 * Requires:
 * 	sp must point to an initialized sbuf_t instance.
 * 	The buffer must not be empty at the time of call (handled internally).
 *
 * Effects:
 * 	Removes and returns the item at the front of the buffer.
 * 	Blocks if the buffer is empty until an item becomes available.
 * 	Signals potentially waiting producers via the notFull condition variable
 * once an item is removed. Ensures that access to the buffer is thread-safe
 * using mutexes.
 */
int
sbuf_remove(sbuf_t *sp)
{
	int item;
	pthread_mutex_lock(&sp->mutex);
	while (sp->front == sp->rear) { // Buffer is empty
		pthread_cond_wait(&sp->notEmpty, &sp->mutex);
	}
	sp->front = (sp->front + 1) % sp->n;
	item = sp->buf[sp->front];
	pthread_cond_signal(&sp->notFull);
	pthread_mutex_unlock(&sp->mutex);
	return item;
}

/**
 * Requires:
 * 	The vargp pointer must be a valid, non-NULL pointer to an initialized
 * sbuf_t instance.
 *
 * Effects:
 * 	Continuously removes connection file descriptors from the buffer using
 * sbuf_remove. Processes each connection by calling doit. Operates in a loop
 * that only terminates externally.
 */
void *
thread_function(void *vargp)
{
	sbuf_t *sp = (sbuf_t *)vargp;
	while (1) {
		int connfd = sbuf_remove(sp); // Remove connection from buffer.
		doit(connfd);		      // Pass connfd into doit.
	}
	return NULL;
}

/*
 * Requires:
 *   	argc equals 2.
 *   	argv[1] must be a valid port number.
 *
 * Effects:
 *   	Initializes and starts a multithreaded HTTP proxy server that listens on
 * the specified port. Logs all requests to a file named "proxy.log". Repeatedly
 * accepts incoming connections. Each connection is processed by worker threads
 * that retrieve connection file descriptors from a shared, thread-safe buffer.
 *   	Ignores broken pipe.
 */
int
main(int argc, char **argv)
{
	int listenfd;
	socklen_t clientlen;
	struct sockaddr_in clientaddr;
	pthread_t tid;

	/* Check the arguments. */
	if (argc != 2) {
		fprintf(stderr, "Usage: %s <port>\n", argv[0]);
		exit(1);
	}

	/* Ignore broken pipe signals. */
	Signal(SIGPIPE, SIG_IGN);

	/* Open a file to log all requests. */
	log_file = fopen("proxy.log", "a");

	/* Initialize log mutex and shared buffer */
	pthread_mutex_init(&log_mutex, NULL);
	sbuf_t conn_buf;
	sbuf_init(&conn_buf, SBUFSIZE);

	/* Open a listening socket */
	listenfd = Open_listenfd(argv[1]);

	for (int i = 0; i < NTHREADS; i++) {
		pthread_create(&tid, NULL, thread_function, (void *)&conn_buf);
	}

	while (1) {
		// Single Thread Implementation
		// clientlen = sizeof(clientaddr);
		// int *connfdp = malloc(sizeof(int)); // Allocate space to
		// avoid race condition *connfdp = Accept(listenfd, (SA
		// *)&clientaddr, &clientlen); // 		/* Repeatedly accept
		// a connection request */ pthread_create(&tid, NULL, doit,
		// connfdp); // Create a new thread to handle the request

		clientlen = sizeof(clientaddr);
		int connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);

		/* Insert accepted connection descriptor into buffer */
		sbuf_insert(&conn_buf, connfd);
	}
	// fclose(log_file);
	// fprintf(stdout, "The log file is closed.");
	return (0);
}
/**
 * Requires:
 * 	connfd is a valid socket file descriptor representing an open connection
 * to a client.
 *
 * Effects:
 * 	Processes an HTTP GET request.
 * 	Parses the URI.
 * 	Forwards the parsed request to the target server specified in the URI.
 * 	Retrieves the response from the target server and forwards it back to
 * the client. Logs the completed transaction details to a log file.
 */
void *
doit(int connfd)
{

	char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
	char *entry;
	rio_t rio, server_rio;
	int bytes_forwarded = 0;
	ssize_t n;
	ssize_t result;

	// Read request line and header.
	rio_readinitb(&rio, connfd);

	// Read the first request line
	if ((n = rio_readlineb(&rio, buf, MAXLINE)) < 0) {
		client_error(connfd, buf, 400, "Bad request",
		    "Request cannot be read by server.");
		return NULL;
	}

	// Fill in the method, URI, and version info from the first header.
	// Write up 2
	if (sscanf(buf, "%s %s %s", method, uri, version) != 3) {
		client_error(connfd, buf, 400, "Bad request",
		    "Request cannot be read by server.");
		return NULL;
	}

	// Only handle GET requests
	if (strcasecmp(method, "GET")) {
		client_error(connfd, method, 501, "Not Implemented",
		    "Proxy does not implement this method");
		return NULL;
	}

	// Parse URI
	char *hostname = NULL, *port = NULL, *pathname = NULL;
	if (parse_uri(uri, &hostname, &port, &pathname) < 0) {
		client_error(connfd, uri, 404, "Not found",
		    "Proxy unable to read URI.");
		return NULL;
	}

	/* Get the client's socket address */
	struct sockaddr_in clientaddr;
	socklen_t clientlen = sizeof(clientaddr);
	if (getpeername(connfd, (SA *)&clientaddr, &clientlen) < 0) {
		fprintf(stderr, "getpeername failed: %s\n", strerror(errno));
		return NULL;
	}

	// Connect to the server and forward the request
	int serverfd = open_clientfd(hostname, port);
	if (serverfd < 0) {
		client_error(connfd, hostname, 404, "Not found",
		    "Proxy unable to connect to the server");
		free(hostname);
		free(port);
		free(pathname);
		return NULL;
	}

	// Send the request header to the server.
	// sprintf(buf, "GET %s %s\r\n", pathname, version);

	if ((rio_writen(serverfd, buf, strlen(buf))) < 0) {
		client_error(serverfd, buf, 400, "Bad request",
		    "Request cannot be read by server.");
		return NULL;
	}

	// Read all the header from client and write to server.
	// Write up 3.
	rio_readinitb(&server_rio, serverfd);
	while ((n = rio_readlineb(&rio, buf, MAXLINE)) > 0) {
		if (!strcmp(buf, "\r\n")) { // Break as soon as received all the
					    // header info.
			break;
		} else if (!strncmp(buf, "Connection",
			       10)) { // Strip the Connection header.
			printf("%s", buf);
			continue;
		} else if (!strncmp(buf, "Keep-Alive",
			       10)) { // Strip the Keep-Alive header.
			printf("%s", buf);
			continue;
		} else if (!strncmp(buf, "Proxy-Connection",
			       16)) { // Strip the Proxy-Connection header.
			printf("%s", buf);
			continue;
		} else { // Otherwise, just send the header to the server.
			printf("%s", buf);
			if ((rio_writen(serverfd, buf, n)) < 0) {
				client_error(serverfd, buf, 400, "Bad request",
				    "Request could not be understood by the server.");
				return NULL;
			}
		}
	}

	// Check if version is HTTP/1.1
	if (strcmp(version, "HTTP/1.1")) {
		char *last_header = "Connection: close\r\n";
		rio_writen(serverfd, last_header, strlen(last_header));
		printf("%s", last_header);
	}

	// Print \r\n to signal end of request
	sprintf(buf, "\r\n");
	rio_writen(serverfd, buf, strlen(buf));
	printf("%s", buf);
	printf("*** End of Request ***\n");

	// Read the server's response and forward it to the client
	// Write up 4
	while ((n = rio_readlineb(&server_rio, buf, MAXLINE)) != 0) {
		result = rio_writen(connfd, buf, n);
		if (result < 0) {
			// Handle possible write errors
			if (errno == EPIPE) {
				// fprintf(stderr, "Broken pipe when writing to
				// client.\n");
				client_error(connfd, hostname, 504,
				    "Gateway Timeout",
				    "Error writing request to server");
				break;
			} else if (errno == ECONNRESET) {
				client_error(connfd, hostname, 504,
				    "Gateway Timeout",
				    "Error reading response from server");
				break;
			}
		}
		bytes_forwarded += n;
	}

	// Lock the buffer while you access the thread's logging info.
	pthread_mutex_lock(&threadlock);

	// Log the transaction
	entry = create_log_entry(&clientaddr, uri, bytes_forwarded);
	strcat(entry, "\n");
	fwrite(entry, sizeof(char), strlen(entry), log_file);
	fflush(log_file);

	// Unlock buffer
	pthread_mutex_unlock(&threadlock);

	// Cleanup
	Free(entry);
	Close(serverfd);
	free(hostname);
	free(port);
	free(pathname);
	Close(connfd);
	return NULL;
}

/*
 * Requires:
 *   The parameter "uri" must point to a properly NUL-terminated string.
 *
 * Effects:
 *   Given a URI from an HTTP proxy GET request (i.e., a URL), extract the
 *   host name, port, and path name.  Create strings containing the host name,
 *   port, and path name, and return them through the parameters "hostnamep",
 *   "portp", "pathnamep", respectively.  (The caller must free the memory
 *   storing these strings.)  Return -1 if there are any problems and 0
 *   otherwise.
 */
static int
parse_uri(const char *uri, char **hostnamep, char **portp, char **pathnamep)
{
	const char *pathname_begin, *port_begin, *port_end;

	if (strncasecmp(uri, "http://", 7) != 0)
		return (-1);

	/* Extract the host name. */
	const char *host_begin = uri + 7;
	const char *host_end = strpbrk(host_begin, ":/ \r\n");
	if (host_end == NULL)
		host_end = host_begin + strlen(host_begin);
	int len = host_end - host_begin;
	char *hostname = Malloc(len + 1);
	strncpy(hostname, host_begin, len);
	hostname[len] = '\0';
	*hostnamep = hostname;

	/* Look for a port number.  If none is found, use port 80. */
	if (*host_end == ':') {
		port_begin = host_end + 1;
		port_end = strpbrk(port_begin, "/ \r\n");
		if (port_end == NULL)
			port_end = port_begin + strlen(port_begin);
		len = port_end - port_begin;
	} else {
		port_begin = "80";
		port_end = host_end;
		len = 2;
	}
	char *port = Malloc(len + 1);
	strncpy(port, port_begin, len);
	port[len] = '\0';
	*portp = port;

	/* Extract the path. */
	if (*port_end == '/') {
		pathname_begin = port_end;
		const char *pathname_end = strpbrk(pathname_begin, " \r\n");
		if (pathname_end == NULL)
			pathname_end = pathname_begin + strlen(pathname_begin);
		len = pathname_end - pathname_begin;
	} else {
		pathname_begin = "/";
		len = 1;
	}
	char *pathname = Malloc(len + 1);
	strncpy(pathname, pathname_begin, len);
	pathname[len] = '\0';
	*pathnamep = pathname;

	return (0);
}

/*
 * Requires:
 *   The parameter "sockaddr" must point to a valid sockaddr_in structure.  The
 *   parameter "uri" must point to a properly NUL-terminated string.
 *
 * Effects:
 *   Returns a string containing a properly formatted log entry.  This log
 *   entry is based upon the socket address of the requesting client
 *   ("sockaddr"), the URI from the request ("uri"), and the size in bytes of
 *   the response from the server ("size").
 */
static char *
create_log_entry(const struct sockaddr_in *sockaddr, const char *uri, int size)
{
	struct tm result;

	/*
	 * Create a large enough array of characters to store a log entry.
	 * Although the length of the URI can exceed MAXLINE, the combined
	 * lengths of the other fields and separators cannot.
	 */
	const size_t log_maxlen = MAXLINE + strlen(uri);
	char *const log_str = Malloc(log_maxlen + 1);

	/* Get a formatted time string. */
	time_t now = time(NULL);
	int log_strlen = strftime(log_str, MAXLINE,
	    "%a %d %b %Y %H:%M:%S %Z: ", localtime_r(&now, &result));

	/*
	 * Convert the IP address in network byte order to dotted decimal
	 * form.
	 */
	Inet_ntop(AF_INET, &sockaddr->sin_addr, &log_str[log_strlen],
	    INET_ADDRSTRLEN);
	log_strlen += strlen(&log_str[log_strlen]);

	/*
	 * Assert that the time and IP address fields occupy less than half of
	 * the space that is reserved for the non-URI fields.
	 */
	assert(log_strlen < MAXLINE / 2);

	/*
	 * Add the URI and response size onto the end of the log entry.
	 */
	snprintf(&log_str[log_strlen], log_maxlen - log_strlen, " %s %d", uri,
	    size);

	return (log_str);
}

/*
 * Requires:
 *   The parameter "fd" must be an open socket that is connected to the client.
 *   The parameters "cause", "short_msg", and "long_msg" must point to properly
 *   NUL-terminated strings that describe the reason why the HTTP transaction
 *   failed.  The string "short_msg" may not exceed 32 characters in length,
 *   and the string "long_msg" may not exceed 80 characters in length.
 *
 * Effects:
 *   Constructs an HTML page describing the reason why the HTTP transaction
 *   failed, and writes an HTTP/1.0 response containing that page as the
 *   content.  The cause appearing in the HTML page is truncated if the
 *   string "cause" exceeds 2048 characters in length.
 */
static void
client_error(int fd, const char *cause, int err_num, const char *short_msg,
    const char *long_msg)
{
	char body[MAXBUF], headers[MAXBUF], truncated_cause[2049];

	assert(strlen(short_msg) <= 32);
	assert(strlen(long_msg) <= 80);
	/* Ensure that "body" is much larger than "truncated_cause". */
	assert(sizeof(truncated_cause) < MAXBUF / 2);

	/*
	 * Create a truncated "cause" string so that the response body will not
	 * exceed MAXBUF.
	 */
	strncpy(truncated_cause, cause, sizeof(truncated_cause) - 1);
	truncated_cause[sizeof(truncated_cause) - 1] = '\0';

	/* Build the HTTP response body. */
	snprintf(body, MAXBUF,
	    "<html><title>Proxy Error</title><body bgcolor=\"ffffff\">\r\n"
	    "%d: %s\r\n"
	    "<p>%s: %s\r\n"
	    "<hr><em>The COMP 321 Web proxy</em>\r\n",
	    err_num, short_msg, long_msg, truncated_cause);

	/* Build the HTTP response headers. */
	snprintf(headers, MAXBUF,
	    "HTTP/1.0 %d %s\r\n"
	    "Content-type: text/html\r\n"
	    "Content-length: %d\r\n"
	    "\r\n",
	    err_num, short_msg, (int)strlen(body));

	/* Write the HTTP response. */
	if (rio_writen(fd, headers, strlen(headers)) != -1)
		rio_writen(fd, body, strlen(body));
}

// Prevent "unused function" and "unused variable" warnings.
static const void *dummy_ref[] = { client_error, create_log_entry, dummy_ref,
	parse_uri };