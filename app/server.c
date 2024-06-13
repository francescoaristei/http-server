#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

# define BUF_SIZE 8192
# define MAX_LINE 4096

/* struct that defines an internal buffer where to read/write avoiding frequent traps to OS */
typedef struct {
	int rio_fd;
	int rio_cnt;
	char buf[BUF_SIZE];
	char *rio_bufptr;
} rio_t;


/* initialize rio_t internal buffer */
void rio_init (rio_t *riot, int fd) {
	riot->rio_fd = fd;
	riot->rio_cnt = 0;
	riot->rio_bufptr = riot->buf;
}

/* buffered version of read(), reads from the internal buffer rio_t -> buf until is not completely consumed */
ssize_t rio_read (rio_t *riot, char *usrbuf, size_t n) {
	int cnt;
	while (riot->rio_cnt <= 0) {
		riot->rio_cnt = read(riot->rio_fd, riot->buf, sizeof(riot->buf));
		if (riot->rio_cnt < 0) {
			if (errno != EINTR) { // sighandler
				return -1;
			} 
		} else if (riot->rio_cnt == 0) { // EOF
			return 0;
		} else {
			// reset buffer
			riot->rio_bufptr = riot->buf;
		}
	}

	cnt = n;
	if (riot->rio_cnt < n) {
		cnt = riot->rio_cnt;
	}
	memcpy(usrbuf, riot->rio_bufptr, cnt);
	riot->rio_bufptr += cnt;
	riot->rio_cnt -= cnt;
	return cnt;
}

/* improved version of read, avoids short count, reads until requested bytes are read (n) or EOF */
size_t rio_readnb (rio_t  *riot, void *usrbuf, size_t n) {
	size_t nleft = n;
	ssize_t nread;
	char *buf = usrbuf;

	while (nleft > 0) {
		nread = rio_read(riot, buf, nleft);
		if (nread < 0) {
			if (errno == EINTR) { // interrupted by sighandler, call read() again
				nread = 0;
			} else {
				return -1; // error
			}
		} else if (nread == 0)  { // EOF
			break;
		}
		nleft -= nread;
		buf += nread;
	}

	return (n - nleft);
}


/* buffered write, used to avoid dealing with short counts encountered in network applications due to network delay etc. */
ssize_t rio_writen (int fd, void *buf, size_t n) {
	size_t nleft = n;
	ssize_t nwritten;

	while (nleft > 0) {
		if ((nwritten = write(fd, buf, nleft)) <= 0) {
			if (errno == EINTR)
				nwritten = 0;
			else
				return - 1;
		}
		nleft -= nwritten;
		buf += nwritten;
	}

	return n;
}


void parseRequest (char *requestBuf, char *responseBuf) {
	char path[MAX_LINE];
	int startPath = 0;
	int read = 1; 
	int i_request = 0;
	int i_path = 0;
	while (read) {
		if (requestBuf[i_request] == '/')
			startPath = 1;
		if (startPath)
			path[i_path++] = requestBuf[i_request];
		if (startPath && requestBuf[i_request] == ' ')
			read = 0;
		i_request++;
	}

	// null terminate
	path[i_path] = '\0';

	if (strcmp(path, '/') == 0)
		strcpy(responseBuf, "HTTP/1.1 200 OK\r\n\r\n");
	else
		strcpy(responseBuf, "HTTP/1.1 404 Not Found\r\n\r\n");
}


int main () {
	// Disable output buffering
	setbuf(stdout, NULL);
 	setbuf(stderr, NULL);

	// internal buffer to read from client
	rio_t riot;
	// number of bytes read
	size_t n;

	// buffer to read request
	char bufRequest[MAX_LINE];
	char bufResponse[MAX_LINE];

	int server_fd, client_addr_len, conn_fd;
	struct sockaddr_in client_addr;
	
	server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd == -1) {
		printf("Socket creation failed: %s...\n", strerror(errno));
	 	return 1;
	}
	
	// SO_REUSEADDR ensures that we don't run into 'Address already in use' errors
	int reuse = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
		printf("SO_REUSEADDR failed: %s \n", strerror(errno));
	 	return 1;
	}
	
	struct sockaddr_in serv_addr = { .sin_family = AF_INET ,
	 								 .sin_port = htons(4221),
	 								 .sin_addr = { htonl(INADDR_ANY) },
	 								};
	
	if (bind(server_fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) != 0) {
	 	printf("Bind failed: %s \n", strerror(errno));
	 	return 1;
	}
	
	int connection_backlog = 5;
	if (listen(server_fd, connection_backlog) != 0) {
	 	printf("Listen failed: %s \n", strerror(errno));
	 	return 1;
	}
	
	printf("Waiting for a client to connect...\n");
	client_addr_len = sizeof(client_addr);
	
	conn_fd = accept(server_fd, (struct sockaddr *) &client_addr, &client_addr_len);
	printf("Client connected\n");

	// initialize internal buffer to read from conn_fd
	rio_init(&riot, conn_fd);

	// read request into bufRequest
	while ((n = rio_readnb(&riot, bufRequest, MAX_LINE - 1)) != 0) {
		printf("%d bytes read by the server\n", n);
	}

	bufRequest[n] = '\0';
	parseRequest(bufRequest, bufResponse);

	strcpy(bufResponse, "AAAAAAA");

	ssize_t nres = rio_writen(conn_fd, bufResponse, strlen(bufResponse));
	
	close(conn_fd);
	close(server_fd);

	return 0;
}
