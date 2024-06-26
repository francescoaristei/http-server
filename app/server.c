# include <stdio.h>
# include <stdlib.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <netinet/ip.h>
# include <string.h>
# include <errno.h>
# include <semaphore.h>
# include <pthread.h>
# include <fcntl.h>
# include <unistd.h>
# include <zlib.h>

# define BUF_SIZE 512
# define MAX_LINE 256
# define PATH 64
# define HEADERS 8
# define WORKER_THREADS 4
# define SBUFSIZE 16

/* bounded buffer to implement producer-consumer pattern with worker threads */
typedef struct {
    int *buf; /* shared buffer between producer and consumer */
    int n; /* max number of slots */
    int front; /* buf[(front+1)%n] is the first item */
    int rear; /* buf[(rear%n)] is the last item */
    sem_t mutex; /* protects buf */
    sem_t slots;
    sem_t items;
} sbuf_t;


/* initialize sbuf_t */
void sbuf_init (sbuf_t *sp, int n) {
    sp -> buf = calloc(n, sizeof(int));
    sp -> n = n;
    sp -> front = sp -> rear = 0;
    sem_init(&sp-> mutex, 0, 1);
    sem_init (&sp->slots, 0, n);
    sem_init(&sp->items, 0, 0);
}

void sbuf_deinit (sbuf_t *sp) {
    free(sp->buf);
}


void sbuf_insert (sbuf_t *sp, int item) {
    sem_wait(&sp->slots); /* decrement slots --> new item is inserted, -1 available slot */
    sem_wait(&sp->mutex);
    sp->buf[(++sp->rear) % (sp->n)] = item;
    sem_post(&sp->mutex);
    sem_post(&sp->items); /* increment items --> new item is inserted, +1 available item to consume */
}

int sbuf_remove (sbuf_t *sp) {
    int item;
    sem_wait(&sp->items); /* decrements items --> item removed, -1 available item to consume */
    sem_wait(&sp->mutex);
    item = sp->buf[(++sp->front) % (sp->n)];
    sem_post(&sp->mutex);
    sem_post(&sp->slots); /* increment slots --> item removed, +1 available slot for new items */
    return item;
}

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
        /* careful: if there is no input to read anymore (all consumed at previous iterations), it blocks */
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
    int i = 0, j = 0;

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

int gzip (char *input, size_t input_len, char *output, size_t *output_len) {
    int ret;
    z_stream stream;

    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;

    /* initialize zlib stream for compression */
    ret = deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 0x1F, 8, Z_DEFAULT_STRATEGY);

    if (ret != Z_OK) {
        return ret;
    }

    /* set input and output */
    stream.avail_in = input_len;
    stream.next_in = (unsigned char*)input;
    stream.avail_out = *output_len;
    stream.next_out = (unsigned char*)output;

    /* compress */
    ret = deflate(&stream, Z_FINISH);
    if (ret != Z_STREAM_END) {
        deflateEnd(&stream);
        return ret == Z_OK ? Z_BUF_ERROR : ret;
    }

    *output_len = stream.total_out;

    /* clean up */
    deflateEnd(&stream);

    return Z_OK;
}


/* echo endpoint */
void echo_endpoint (char *bufResponse, char *ptr, char *response, char *encoding, int *resp_len) {
    int len = strlen("echo");
    ptr += len;
    int i;
    for (i = 0; *ptr != '\0'; i++) {
        response[i] = *++ptr;
    }
    response[i--] = '\0';

    if (encoding != NULL) {
        char *ch = strchr(encoding, ' ');
        int j = 0;
        char type_encoding[MAX_LINE];

        while (*ch != '\r') {
            while (*++ch != ',' && *ch != '\r')
                if (*ch != ' ') type_encoding[j++] = *ch;
            type_encoding[j] = '\0';
            if (strcmp(type_encoding, "gzip") == 0)
                break;
            j = 0;
        }
        
        if (strcmp(type_encoding, "gzip") == 0) {
            char compressed[MAX_LINE];
            size_t compressed_length = sizeof(compressed);
            size_t response_length = strlen(response);
            if (gzip(response, response_length, compressed, &compressed_length) == Z_OK) {
                sprintf(bufResponse, "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n\r\n", compressed_length);
                memcpy(bufResponse + strlen(bufResponse), compressed, compressed_length);
                *resp_len = strlen(bufResponse) + compressed_length;
            }
        } else {
            sprintf(bufResponse, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n%s", i, response);
            *resp_len = strlen(bufResponse);
        }
    }
    else {
        sprintf(bufResponse, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n%s", i, response);
        *resp_len = strlen(bufResponse);
    }
}


void useragent_endpoint (char *bufResponse, char *useragent, char *response) {
    char *ch = strchr(useragent, ' ');
    int i;
    for (i = 0; *ch != '\r'; i++) {
        response[i] = *++ch;
    }

    response[i--] = '\0';

    sprintf(bufResponse, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n%s", i, response);
}


char *dir;

void get_file_endpoint (char *bufResponse, char *path, char *response) {
    int fd;
    char c;
    char filename[PATH];
    int i = 0, j = 0, t = 0;

    char complete_path[PATH];

    char *ptr = strchr(path, '/');
    while (*ptr != '\0')
        filename[j++] = *++ptr;

    filename[j] = '\0';

    char *fileptr = filename;
    char *dirptr = dir;
    while (*dirptr != '\0')
        complete_path[t++] = *dirptr++;
    while (*fileptr != '\0')
        complete_path[t++] = *fileptr++;
    complete_path[t] = '\0';
    
    if ((fd = open(complete_path, O_RDONLY, 0)) == -1) {
        printf("Error opening the file.\n");
        sprintf(bufResponse, "HTTP/1.1 404 Not Found\r\n\r\n");
        return;
    }
    while (read(fd, &c, 1) != 0)
        response[i++] = c;

    response[i] = '\0';

    if (close(fd) == -1) {
        printf("Error closing file.\n");
        return 1;
    }
    sprintf(bufResponse, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n%s", i, response);
}


void post_file_endpoint (char *bufResponse, char *path, char *response, char *body, char *req_type, char *req_length) {
    int fd;
    char c;
    char filename[PATH];

    char complete_path[PATH];

    int i = 0, j = 0, z = 0, t = 0;

    char *ptr = strchr(path, '/');
    while (*ptr != '\0')
        filename[j++] = *++ptr;

    filename[j] = '\0';

    char *fileptr = filename;
    char *dirptr = dir;
    while (*dirptr != '\0')
        complete_path[t++] = *dirptr++;
    while (*fileptr != '\0')
        complete_path[t++] = *fileptr++;
    complete_path[t] = '\0';
    
    if ((fd = open(complete_path, O_WRONLY | O_APPEND | O_CREAT, 0)) == -1) {
        printf("Error creating the file.\n");
        strcpy(bufResponse, "HTTP/1.1 404 Not Found\r\n\r\n");
        return;
    }

    while (body[z] != '\0') {
        write(fd, &body[z], 1);
        response[i++] = body[z++];
    }

    response[i] = '\0';

    if (close(fd) == -1) {
        printf("Error closing file.\n");
        return 1;
    }

    strcpy(bufResponse, "HTTP/1.1 201 Created\r\n\r\n");
}


/* shared buffer of descriptors */
sbuf_t sbuf;
void *thread (void *vargv);

int main (int argc, char *argv[]) {

    /* Disable output buffering */
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    /* common to all threads */
    pthread_t tid;

    /* initialize producer-consumer buffer */
    sbuf_init(&sbuf, SBUFSIZE);

    /* save the dir in case of /files endpoint */
    if (argc == 3) {
        if (strcmp(argv[1], "--directory") == 0) {
            dir = argv[2];
            printf("dir: %s\n", dir);
        }
    }

    int server_fd, client_addr_len, conn_fd;
    struct sockaddr_in client_addr;
    
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        printf("Socket creation failed: %s...\n", strerror(errno));
        return 1;
    }
    
    /* SO_REUSEADDR ensures that we don't run into 'Address already in use' errors */
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

    for (int i = 0; i < WORKER_THREADS; i++) {
        pthread_create(&tid, NULL, thread, NULL);
    }
    
    while (1) {
        printf("Waiting for a client to connect...\n");
        client_addr_len = sizeof(client_addr);
        conn_fd = accept(server_fd, (struct sockaddr *) &client_addr, &client_addr_len);
        if (conn_fd == -1) {
            printf("Client not connected, exiting...");
            return 1;
        }
        printf("Client connected\n");
        sbuf_insert(&sbuf, conn_fd);
    }

    close(server_fd);
    return 0;
}

void response (int conn_fd);

void *thread (void *vargv) {
    pthread_detach(pthread_self());
    int conn_fd = sbuf_remove(&sbuf);
    response(conn_fd);
    close(conn_fd);
}

sem_t files_mutex; 

void response (int conn_fd) {

    /* internal buffer to read from client */
    rio_t riot;

    /* number of bytes read */
    size_t n;

    /* buffer to read request */
    char bufRequest[MAX_LINE];
    char bufResponse[MAX_LINE];
    char body[MAX_LINE];
    char path[PATH];
    char headers[HEADERS][MAX_LINE];
    char response[MAX_LINE];
    char true_path[PATH];

    /* initialize internal buffer to read from conn_fd */
    rio_init(&riot, conn_fd);
    
    // read request into bufRequest
	int request_complete = 0;
    int is_body = 0;
	int total_read = 0;
    int is_header = 0;
    int header_count = 0;
    size_t bufLength = MAX_LINE;
    char bodyLength[MAX_LINE];

    sem_init(&files_mutex, 0, 1); // binary mutex (TO-DO: change in the reader-writer paradigm?)

	while (1) {
        /* avoid blocking on read() */
        if (is_body) {
            sscanf(bodyLength, "%llu", &bufLength);
            /* needed for rio_readlineb */
            bufLength += 1;
        }

        n = rio_readlineb(&riot, bufRequest, bufLength);
		if (n < 0) {
			printf("Error reading...\n");
			return 1;
		}

        /* TO-DO: understand how to handle */
        if (n == 0) {
        }

        bufRequest[n] = '\0';


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
                    if (strstr(headers[i], "Content-Length:") != NULL) {
                        char *ptr = strchr(headers[i], ' ');
                        int count = 0;
                        /* body length */
                        while (*ptr != '\r')
                            bodyLength[count++] = *++ptr;
                        bodyLength[count] = '\0';
                        is_body = 1;
                        break; /* exit for loop */
                    }
                }
                if (is_body)
                    continue;

                break; /* no body */
            }
            strcpy(headers[header_count++], bufRequest);
            
            printf("The header n. %d is: %s\n", header_count, bufRequest);
        }

        /* body */
        if (is_body) {
            strcpy(body, bufRequest);
            printf("The body is: %s\n", body);
            break;
        }
	}

    find_path(path, true_path);
    char *path_ptr;
    int resp_len;

    if ((path_ptr = strstr(true_path, "echo")) != NULL) {
        char *enc;
        for (int i = 0; i < header_count; i++) {
            if ((enc = strstr(headers[i], "Accept-Encoding")) != NULL) {
                break;
            }
        }
        echo_endpoint(bufResponse, path_ptr, response, enc, &resp_len);
        if (enc != NULL)
            /* write gives gzip:invalid header */
            send(conn_fd, bufResponse, resp_len, 0);

    } else if ((path_ptr = strstr(true_path, "user-agent")) != NULL) {
        char *user_agent;
        for (int i = 0; i < header_count; i++) {
            if ((user_agent = strstr(headers[i], "User-Agent")) != NULL)
                break;
        }
        useragent_endpoint(bufResponse, user_agent, response);
        resp_len = strlen(bufResponse);

    } else if ((path_ptr = strstr(true_path, "files")) != NULL) {
        if (strstr(path, "GET") != NULL) {
            sem_wait(&files_mutex);
            get_file_endpoint(bufResponse, path_ptr, response);
            resp_len = strlen(bufResponse);
            sem_post(&files_mutex);
        } else if (strstr(path, "POST") != NULL) {
            char *req_type;
            char *req_length;
            for (int i = 0; i < header_count; i++) {
                if ((req_type = strstr(headers[i], "Content-Type")) != NULL) {
                    break;
                }
            }
            for (int i = 0; i < header_count; i++) {
                if ((req_length = strstr(headers[i], "Content-Length")) != NULL) {
                    break;
                }
            }
            sem_wait(&files_mutex);
            post_file_endpoint(bufResponse, path_ptr, response, body, req_type, req_length);
            resp_len = strlen(bufResponse);
            sem_post(&files_mutex);
        }
    } else {
        if (strcmp(true_path, "/") == 0) {
            strcpy(bufResponse, "HTTP/1.1 200 OK\r\n\r\n");
        }
        else {
            strcpy(bufResponse, "HTTP/1.1 404 Not Found\r\n\r\n");
        }//
        resp_len = strlen(bufResponse);
    }
    ssize_t nres = rio_writen(conn_fd, bufResponse, resp_len);
}