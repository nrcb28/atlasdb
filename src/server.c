#define _POSIX_C_SOURCE 200809L
#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* A detached worker owns its connection and session. The server owns the
 * worker registry until every worker has signalled its departure. */
typedef struct Server Server;
typedef struct { Server *server; int fd; size_t slot; } Worker;
struct Server {
    ADBDatabase *database;
    pthread_mutex_t mutex;
    pthread_cond_t finished;
    Worker **workers;
    size_t capacity, active;
    atomic_bool stopping;
};

/* C permits lock-free atomics in signal handlers. This also covers a signal
 * delivered to a worker while the listener thread observes the stop flag. */
_Static_assert(ATOMIC_BOOL_LOCK_FREE == 2, "AtlasDB needs lock-free atomic bool");
static atomic_bool server_stop;
static void stop_server(int signal_number) {
    (void)signal_number;
    atomic_store_explicit(&server_stop, true, memory_order_relaxed);
}

static bool send_all(int fd, const char *data, size_t length) {
    while (length) {
        ssize_t count = send(fd, data, length, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        data += (size_t)count;
        length -= (size_t)count;
    }
    return true;
}

static char *error_json(const ADBError *error) {
    ADBBuffer buffer = {0};
    adb_buffer_printf(&buffer, "{\"ok\":false,\"code\":%d,\"error\":", (int)error->code);
    adb_buffer_json_string(&buffer, error->message);
    adb_buffer_printf(&buffer, ",\"offset\":%zu}", error->offset);
    if (buffer.failed) { adb_buffer_free(&buffer); return NULL; }
    return buffer.data;
}

static bool send_error(int fd, ADBCode code, const char *message) {
    ADBError error = {.code = code};
    (void)snprintf(error.message, sizeof(error.message), "%s", message);
    char *json = error_json(&error);
    if (!json) return false;
    bool success = send_all(fd, json, strlen(json)) && send_all(fd, "\n", 1);
    free(json);
    return success;
}

static bool execute_line(int fd, ADBSession *session, const char *sql) {
    ADBResult result = {0};
    ADBError error = {0};
    bool success = adb_execute(session, sql, &result, &error);
    char *json = success ? adb_result_json(&result) : error_json(&error);
    adb_result_free(&result);
    if (!json) return send_error(fd, ADB_ERR_MEMORY, "No hay memoria para la respuesta");
    bool sent;
    if (success && json[0] == '{') {
        sent = send_all(fd, "{\"ok\":true,", 11) &&
               send_all(fd, json + 1, strlen(json) - 1) && send_all(fd, "\n", 1);
    } else {
        sent = send_all(fd, json, strlen(json)) && send_all(fd, "\n", 1);
    }
    free(json);
    return sent;
}

static time_t monotonic_seconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return time(NULL);
    return value.tv_sec;
}

static bool host_is_loopback(const char *host) {
    if (!host || !*host) return false;
    struct addrinfo hints = {0}, *addresses = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int status = getaddrinfo(host, NULL, &hints, &addresses);
    if (status != 0 || !addresses) return false;
    bool loopback = true;
    for (const struct addrinfo *address = addresses; address; address = address->ai_next) {
        if (address->ai_family == AF_INET) {
            const struct sockaddr_in *v4 = (const struct sockaddr_in *)address->ai_addr;
            if ((ntohl(v4->sin_addr.s_addr) & UINT32_C(0xff000000)) != UINT32_C(0x7f000000)) {
                loopback = false;
                break;
            }
        } else if (address->ai_family == AF_INET6) {
            const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)address->ai_addr;
            if (!IN6_IS_ADDR_LOOPBACK(&v6->sin6_addr)) {
                loopback = false;
                break;
            }
        } else {
            loopback = false;
            break;
        }
    }
    freeaddrinfo(addresses);
    return loopback;
}

static void *serve_connection(void *argument) {
    Worker *worker = argument;
    Server *server = worker->server;
    ADBError error = {0};
    ADBSession *session = adb_session_create(server->database, &error);
    char *line = malloc((size_t)ADB_MAX_SQL + 1);
    if (!session || !line) {
        (void)send_error(worker->fd, ADB_ERR_MEMORY, "No hay memoria para la sesion");
    } else {
        char chunk[8192];
        size_t length = 0;
        time_t started = monotonic_seconds();
        bool connected = true;
        while (connected && !atomic_load_explicit(&server->stopping, memory_order_relaxed)) {
            if (monotonic_seconds() - started >= 60) {
                (void)send_error(worker->fd, ADB_ERR_LIMIT, "Tiempo de espera agotado (60 segundos)");
                break;
            }
            ssize_t count = recv(worker->fd, chunk, sizeof(chunk), 0);
            if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if (count <= 0) break;
            for (ssize_t i = 0; i < count; ++i) {
                unsigned char c = (unsigned char)chunk[i];
                if (c == '\n') {
                    if (length && line[length - 1] == '\r') --length;
                    line[length] = '\0';
                    connected = execute_line(worker->fd, session, line);
                    length = 0;
                    started = monotonic_seconds();
                    if (!connected) break;
                } else if (c == '\0') {
                    (void)send_error(worker->fd, ADB_ERR_PARSE, "Un comando no puede contener bytes NUL");
                    connected = false;
                    break;
                } else if (length == ADB_MAX_SQL) {
                    (void)send_error(worker->fd, ADB_ERR_LIMIT, "Comando demasiado largo (limite: 1 MiB)");
                    connected = false;
                    break;
                } else {
                    line[length++] = (char)c;
                }
            }
        }
    }
    free(line);
    adb_session_destroy(session);
    pthread_mutex_lock(&server->mutex);
    server->workers[worker->slot] = NULL;
    close(worker->fd);
    --server->active;
    pthread_cond_signal(&server->finished);
    pthread_mutex_unlock(&server->mutex);
    free(worker);
    return NULL;
}

static int open_socket(const char *host, unsigned port, bool listener) {
    char service[16];
    (void)snprintf(service, sizeof(service), "%u", port);
    struct addrinfo hints = {0}, *addresses = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = listener ? AI_PASSIVE : 0;
    int status = getaddrinfo(host, service, &hints, &addresses);
    if (status) { fprintf(stderr, "Direccion invalida: %s\n", gai_strerror(status)); return -1; }
    int fd = -1;
    for (struct addrinfo *address = addresses; address; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) continue;
        if (listener) {
            int enabled = 1;
            (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
            if (bind(fd, address->ai_addr, address->ai_addrlen) == 0 && listen(fd, 64) == 0) break;
        } else {
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) {
                int connected = connect(fd, address->ai_addr, address->ai_addrlen);
                if (connected < 0 && errno == EINPROGRESS) {
                    struct pollfd connection = {.fd = fd, .events = POLLOUT};
                    int ready;
                    do { ready = poll(&connection, 1, 5000); } while (ready < 0 && errno == EINTR);
                    if (ready > 0) {
                        int socket_error = 0;
                        socklen_t length = sizeof(socket_error);
                        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) == 0 && !socket_error)
                            connected = 0;
                        else if (socket_error) errno = socket_error;
                    } else if (!ready) errno = ETIMEDOUT;
                }
                if (connected == 0 && fcntl(fd, F_SETFL, flags) == 0) break;
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0) perror(listener ? "No se puede iniciar el servidor" : "No se puede conectar");
    return fd;
}

int adb_run_server(ADBDatabase *database, const char *host, unsigned port, size_t max_clients,
                   bool insecure_remote) {
    if (!database || !host || !max_clients || max_clients > ADB_MAX_CLIENTS || !port || port > 65535) return 1;
    if (!insecure_remote && !host_is_loopback(host)) {
        fprintf(stderr, "Por seguridad, el servidor solo puede escuchar en loopback. "
                        "Usa --insecure-remote únicamente detrás de una red protegida.\n");
        return 1;
    }
    int listener = open_socket(host, port, true);
    if (listener < 0) return 1;
    Server server = {.database = database, .capacity = max_clients};
    atomic_init(&server.stopping, false);
    server.workers = calloc(max_clients, sizeof(*server.workers));
    if (!server.workers) { close(listener); return 1; }
    if (pthread_mutex_init(&server.mutex, NULL) != 0) { free(server.workers); close(listener); return 1; }
    if (pthread_cond_init(&server.finished, NULL) != 0) {
        pthread_mutex_destroy(&server.mutex); free(server.workers); close(listener); return 1;
    }
    struct sigaction action = {0}, old_int, old_term, old_pipe;
    action.sa_handler = stop_server;
    sigemptyset(&action.sa_mask);
    atomic_store_explicit(&server_stop, false, memory_order_relaxed);
    (void)sigaction(SIGINT, &action, &old_int);
    (void)sigaction(SIGTERM, &action, &old_term);
    action.sa_handler = SIG_IGN;
    (void)sigaction(SIGPIPE, &action, &old_pipe);
    fprintf(stderr, "AtlasDB %s escuchando en %s:%u (maximo %zu clientes)\n", ADB_VERSION, host, port, max_clients);
    int exit_status = 0;
    while (!atomic_load_explicit(&server_stop, memory_order_relaxed)) {
        struct pollfd poll_fd = {.fd = listener, .events = POLLIN};
        int ready = poll(&poll_fd, 1, 250);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { perror("poll"); exit_status = 1; break; }
        if (!ready) continue;
        if (!(poll_fd.revents & POLLIN)) { exit_status = 1; break; }
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED) continue;
            perror("accept"); exit_status = 1; break;
        }
        struct timeval receive_timeout = {.tv_sec = 1}, send_timeout = {.tv_sec = 5};
        (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
        (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
        pthread_mutex_lock(&server.mutex);
        size_t slot = 0;
        while (slot < max_clients && server.workers[slot]) ++slot;
        if (slot == max_clients) {
            pthread_mutex_unlock(&server.mutex);
            (void)send_error(client, ADB_ERR_LIMIT, "Limite de clientes alcanzado");
            close(client);
            continue;
        }
        Worker *worker = malloc(sizeof(*worker));
        if (!worker) {
            pthread_mutex_unlock(&server.mutex);
            (void)send_error(client, ADB_ERR_MEMORY, "No hay memoria para el cliente"); close(client); continue;
        }
        *worker = (Worker){.server = &server, .fd = client, .slot = slot};
        server.workers[slot] = worker;
        ++server.active;
        pthread_attr_t attributes;
        pthread_t thread;
        int code = pthread_attr_init(&attributes);
        if (code == 0) {
            code = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
            if (code == 0) code = pthread_create(&thread, &attributes, serve_connection, worker);
            pthread_attr_destroy(&attributes);
        }
        if (code != 0) {
            server.workers[slot] = NULL; --server.active; free(worker); close(client);
            fprintf(stderr, "No se pudo iniciar un hilo: %s\n", strerror(code));
        }
        pthread_mutex_unlock(&server.mutex);
    }
    close(listener);
    atomic_store_explicit(&server_stop, true, memory_order_relaxed);
    atomic_store_explicit(&server.stopping, true, memory_order_relaxed);
    pthread_mutex_lock(&server.mutex);
    for (size_t i = 0; i < server.capacity; ++i)
        if (server.workers[i]) (void)shutdown(server.workers[i]->fd, SHUT_RDWR);
    while (server.active) pthread_cond_wait(&server.finished, &server.mutex);
    pthread_mutex_unlock(&server.mutex);
    (void)sigaction(SIGINT, &old_int, NULL);
    (void)sigaction(SIGTERM, &old_term, NULL);
    (void)sigaction(SIGPIPE, &old_pipe, NULL);
    pthread_cond_destroy(&server.finished);
    pthread_mutex_destroy(&server.mutex);
    free(server.workers);
    return exit_status;
}

int adb_run_client(const char *host, unsigned port, const char *sql) {
    if (!sql || strlen(sql) > ADB_MAX_SQL || strchr(sql, '\n') || strchr(sql, '\r')) {
        fprintf(stderr, "El cliente acepta un comando de una linea de hasta 1 MiB.\n"); return 1;
    }
    int fd = open_socket(host, port, false);
    if (fd < 0) return 1;
    struct timeval timeout = {.tv_sec = 65};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (!send_all(fd, sql, strlen(sql)) || !send_all(fd, "\n", 1)) {
        perror("No se pudo enviar el comando"); close(fd); return 1;
    }
    ADBBuffer response = {0};
    char chunk[8192];
    bool complete = false;
    while (!complete) {
        ssize_t count = recv(fd, chunk, sizeof(chunk), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        char *newline = memchr(chunk, '\n', (size_t)count);
        size_t length = newline ? (size_t)(newline - chunk) : (size_t)count;
        if (response.length > ADB_MAX_JSON_BYTES || length > ADB_MAX_JSON_BYTES - response.length ||
            !adb_buffer_append(&response, chunk, length)) break;
        complete = newline != NULL;
    }
    close(fd);
    if (!complete) {
        fprintf(stderr, "Respuesta incompleta o demasiado grande del servidor.\n"); adb_buffer_free(&response); return 1;
    }
    puts(response.data ? response.data : "");
    int exit_status = response.data && strncmp(response.data, "{\"ok\":true", 10) == 0 ? 0 : 1;
    adb_buffer_free(&response);
    return exit_status;
}
