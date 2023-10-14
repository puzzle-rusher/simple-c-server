#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define MAX_EVENTS 32
static char *dir;

struct Args {
    in_port_t port;
    in_addr_t ip_address;
    char *directory;
};

struct Args parse_args(int argc, char *argv[]) {
    int opt;
    struct Args args;
    int count = 0;
    while ((opt = getopt(argc, argv, "h:p:d:")) != -1) {
        count++;
        switch (opt) {
            case 'h':
                args.ip_address = inet_addr(optarg);
                break;
            case 'p':
                args.port = (in_port_t)atoi(optarg);
                break;
            case 'd':
                args.directory = optarg;
                break;
            default:
                fprintf(stderr, "Usage: %s [-t nsecs] [-n] name\n",
                        argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    if (count != 3) {
        perror("not all arguments");
        exit(EXIT_FAILURE);
    }

    return args;
}

int set_nonblock(int fd) {
    int flags;
    if(-1 == (flags = fcntl(fd, F_GETFL, 0)))
        flags = 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void bind_socket(int socket_fd, in_addr_t ip, in_port_t port) {
    struct sockaddr_in addr_params;
    addr_params.sin_family = AF_INET;
    addr_params.sin_addr.s_addr = ip;
    addr_params.sin_port = htons(port);

    if (bind(socket_fd, (struct sockaddr *) &addr_params, sizeof(addr_params)) != 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }
}

void handle_connection(int socket_fd, int epoll_fd) {
    int conn_sock = accept(socket_fd, NULL, NULL);
    if (conn_sock == -1) {
        perror("accept");
        exit(EXIT_FAILURE);
    }
    if (set_nonblock(conn_sock) == -1) {
        perror("set nonblock to connection socket");
        exit(EXIT_FAILURE);
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = conn_sock;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_sock, &ev) == -1) {
        perror("epoll_ctl: conn_sock");
        exit(EXIT_FAILURE);
    }
}

void send_failure_message(int client_fd) {
    char message[] = "HTTP/1.1 404\r\nContent-type: text/html\r\nContent-length: 0\r\n\r\n";

    send(client_fd, message, sizeof(message), MSG_NOSIGNAL);
}

void send_resource(int client_fd, char* resource) {
    char file_name[strlen(dir) + strlen(resource) + 1];
    strcpy(file_name, dir);
    strcat(file_name, resource);
    FILE *file = fopen(file_name, "r");
    if (file == NULL) {
        perror("Error opening the file");
        send_failure_message(client_fd);
        return;
    }

    fseek(file, 0, SEEK_END);
    long fsize = ftell(file);
    fseek(file, 0, SEEK_SET);

    char* http_response;
    asprintf(&http_response, "HTTP/1.1 200\r\nContent-type: text/html\r\nContent-length: %ld\r\n\r\n", fsize);
    size_t headers_len = strlen(http_response);
    size_t response_len = headers_len + fsize;
    char *response = (char *)malloc(response_len);
    strcpy(response, http_response);
    response += headers_len;

    if (response == NULL) {
        perror("Error allocating memory");
        fclose(file);
        return;
    }

    if (fread(response, 1, response_len, file) != fsize) {
        perror("Error reading the file");
    } else {
        response -= headers_len;
        send(client_fd, response, response_len, MSG_NOSIGNAL);
    }

    fclose(file);
    free(response);
}

void find_method_and_subdir(char* src, char **method, char **subdir) {
    size_t buffer_len = strlen(src);
    for (size_t i = 0; i < buffer_len; ++i) {
        if (src[i] == ' ') {
            if (*method == NULL) {
                src[i] = '\0';
                *method = (char *)malloc(i + 1);
                strcpy(*method, src);
            } else if (*subdir == NULL) {
                src[i] = '\0';
                size_t method_size = strlen(*method) + 1;

                *subdir = (char *)malloc(i + 1 - method_size);
                strcpy(*subdir, (src + method_size));

                break;
            }
        }
    }
}

void *handle_message(void *arg) {
    int client_fd = *(int *)arg;
    char buffer[8192];
    char *method = NULL;
    char *subdir = NULL;

    size_t received = recv(client_fd, buffer, 8192, MSG_NOSIGNAL);

    if ((received == 0 || received == -1) && errno != EAGAIN) {
        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
    } else if (received > 0) {
        find_method_and_subdir(buffer, &method, &subdir);

        if (method != NULL && subdir != NULL && strcmp(method, "GET") == 0) {
            send_resource(client_fd, subdir);
        } else {
            send_failure_message(client_fd);
        }

        free(subdir);
        free(method);
        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
    }
}

int true_main(struct Args args)
{
    dir = args.directory;
    int master_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    bind_socket(master_socket_fd, args.ip_address, args.port);
    if (set_nonblock(master_socket_fd) == -1) {
        perror("set nonblock to master socket");
        exit(EXIT_FAILURE);
    }

    listen(master_socket_fd, SOMAXCONN);

    struct epoll_event ev;

    int nfds;
    struct epoll_event events[MAX_EVENTS];
    int epollfd = epoll_create1(0);
    if (epollfd == -1) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    ev.events = EPOLLIN;
    ev.data.fd = master_socket_fd;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, master_socket_fd, &ev) == -1) {
        perror("epoll_ctl: listen_sock");
        exit(EXIT_FAILURE);
    }

    while(1) {
        nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            perror("epoll_wait");
            shutdown(master_socket_fd, SHUT_RDWR);
            close(master_socket_fd);
            exit(EXIT_FAILURE);
        }

        for (int n = 0; n < nfds; ++n) {
            if (events[n].data.fd == master_socket_fd) {
                handle_connection(master_socket_fd, epollfd);
            } else {
                pthread_t thid;
                int client_fd = events[n].data.fd;
                if (pthread_create(&thid, NULL, handle_message, &client_fd) != 0) {
                    perror("pthread_create() error");
                    exit(1);
                }
            }
        }
    }

    shutdown(master_socket_fd, SHUT_RDWR);
    close(master_socket_fd);
    return 0;
}

int main(int argc, char *argv[]) {
    // Создаем дочерний процесс
    pid_t pid = fork();

    // Проверка на ошибку fork
    if (pid < 0) {
        perror("Ошибка при вызове fork");
        exit(1);
    }

    // Если это дочерний процесс
    if (pid == 0) {
        // Создаем новую сессию
        setsid();

        // Изменяем текущий каталог
        chdir("/");

        // Открываем /dev/null и перенаправляем stdin, stdout и stderr на него
        open("/dev/null", O_RDWR); // stdin
        dup(0);                    // stdout
        dup(0);                    // stderr

        // Здесь можно выполнять код демона
        struct Args args = parse_args(argc, argv);
        true_main(args);
    } else {
        // Родительский процесс завершает выполнение
        exit(0);
    }
}
