#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>


# define BUF_SIZE 2048
# define MAX_LINE 1024

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
        // careful: if there is no input to read anymore (all consumed at previous iterations), it blocks
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


ssize_t rio_readlineb (rio_t *rp, void *usrbuf, size_t maxlen) {
	int n, rc;
	char c, *bufp = usrbuf;
	for (n = 1; n < maxlen; n++) {
		if ((rc = rio_read(rp, &c, 1)) == 1) {
			*bufp++ = c;
			if (c == '\n')
				break;
		} else if (rc == 0) {
			if (n == 1)
				return 0; // EOF no data to read
			else
				break; // EOF some data was read
		} else
			return -1; // Error
	}
	*bufp = 0;
	return n;
}

/* buffered write, used to avoid dealing with short counts encountered in network applications due to network delay etc. */
ssize_t rio_writen (int fd, void *buf, size_t n) {
    size_t nleft = n;
    ssize_t nwritten;
    char *bufp = buf;

    printf("%s\n", bufp);

    while (nleft > 0) {
        if ((nwritten = write(fd, bufp, nleft)) <= 0) {
            if (errno == EINTR) {
                printf("EINTR.\n");
                nwritten = 0;
            } else {
                printf("Write Error.\n");
                return - 1;
            }
        }
        nleft -= nwritten;
        bufp += nwritten;
    }

    return n;
}


void find_path (char *path, char *string) {
    int start_path = 0;
    int i, j = 0;

    while (1) {
        if (path[i] == ' ' && !start_path) { /* start space */
            start_path = 1;
            i++;
        }
        if (path[i] == ' ' && start_path) { /* end space */
            string[j] = '\0';
            break;
        }

        if (start_path) {
            string[j++] = path[i];
        }

        i++;
    }
}

/*void parseRequest (char *requestBuf, char *responseBuf) {
    char path[MAX_LINE];
    int startPath = 0;
    int i_request = 0;
    int i_path = 0;

    while (requestBuf[i_request] != '\0' && requestBuf[i_request] != '\r' && requestBuf[i_request] != '\n') {
        if (requestBuf[i_request] == ' ') {
            if (startPath) {
                break;  // end of path
            }
        } else {
            if (requestBuf[i_request] == '/') startPath = 1;
            if (startPath) path[i_path++] = requestBuf[i_request];
        }
        i_request++;
    }


    // null terminate
    path[i_path] = '\0';


    if (strcmp(path, "/") == 0) {
        strcpy(responseBuf, "HTTP/1.1 200 OK\r\n\r\n");
    }
    else {
        strcpy(responseBuf, "HTTP/1.1 404 Not Found\r\n\r\n");
    }
}*/

// echo endpoint
void echo_endpoint (char *path, char *bufResponse) {
    char response[MAX_LINE];
    char true_path[MAX_LINE];
    find_path(path, true_path);
    char *ptr = strstr(true_path, "echo");
    printf("AAA%sAAA\n", true_path);
    if (ptr == NULL) {
        if (true_path == "/") {
            strcpy(bufResponse, "HTTP/1.1 200 OK\r\n\r\n");
            return;
        }
        else {
            strcpy(bufResponse, "HTTP/1.1 404 Not Found\r\n\r\n");
            return;
        }
    }

    int len = strlen("echo");
    ptr += len;
    int i;
    for (i = 0; *ptr != '\0'; i++) {
        
        response[i] = *++ptr;
        printf("%c - %d\n", *ptr, i);
    }

    response[i--] = '\0';

    sprintf(bufResponse, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n%s", i, response);
    printf("%s\n", bufResponse);
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
    char body[MAX_LINE];
    char path[MAX_LINE];
    char headers[MAX_LINE][MAX_LINE];

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
    if (conn_fd == -1) {
        printf("Client not connected, exiting...");
        return 1;
    }

    printf("Client connected\n");

    // initialize internal buffer to read from conn_fd
    rio_init(&riot, conn_fd);
    
    // read request into bufRequest

	int request_complete = 0;
    int is_body = 0;
	int total_read = 0;
    int is_header = 0;
    int header_count = 0;
    //char string[MAX_LINE];

	while (1) {
	    //n = rio_readlineb(&riot, bufRequest + total_read, MAX_LINE);
		//n = rio_readnb(&riot, bufRequest + total_read, MAX_LINE);
        n = rio_readlineb(&riot, bufRequest, MAX_LINE);
		if (n < 0) {
			printf("Error reading...\n");
			return 1;
		}

        /* TO-DO: understand how to handle */
        if (n == 0) {

        }

        //bufRequest[total_read + n] = '\0';
        bufRequest[n] = '\0';

        /*for (int i = total_read; i <= total_read + n; i++) {
            string[i - total_read] = bufRequest[i];
        }*/


		//total_read += n;
		//bufRequest[total_read] = '\0';

        /* path */
        if (!is_header) {
            strcpy(path, bufRequest);
            printf("The path is: %s\n", path);
            is_header = 1;
            continue; /* next iteration */
        }

        /* headers */
        if (is_header && !is_body) {
            /* end of header section */
            if (strcmp(bufRequest, "\r\n") == 0) {
                /* check content length */
                for (int i = 0; i < header_count; i++) {
                    if (strstr("Content-length:", headers[i])) {
                        is_body = 1;
                        continue; /* next iteration */
                    }
                }
                break; /* no body */
            }
            strcpy(headers[header_count++], bufRequest);
            printf("The header n. %d is: %s\n", header_count, bufRequest);
        }

        /* body */
        if (is_body) {
            strcpy(body, bufRequest);
            printf("The body is: %s\n", body); /* empty for now */
            break;
        }
	}

    echo_endpoint(path, bufResponse);

    ssize_t nres = rio_writen(conn_fd, bufResponse, strlen(bufResponse));
    
    close(conn_fd);
    close(server_fd);

    return 0;
}
