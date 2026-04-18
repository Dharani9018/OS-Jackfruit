#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ============================================================================
 * Constants and Configuration
 * ============================================================================ */

#define STACK_SIZE          (1024 * 1024)
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define LOG_DIR             "logs"
#define CONTROL_MESSAGE_LEN 512
#define CHILD_COMMAND_LEN   256
#define LOG_CHUNK_SIZE      4096
#define LOG_BUFFER_CAPACITY 64
#define DEFAULT_SOFT_LIMIT  (40UL << 20)
#define DEFAULT_HARD_LIMIT  (64UL << 20)
#define MONITOR_DEVICE      "/dev/container_monitor"

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef enum {
    EXIT_NORMAL = 0,
    EXIT_SIGNALED,
    EXIT_HARD_LIMIT,
    EXIT_STOP_REQUESTED
} exit_reason_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    exit_reason_t exit_reason;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
    int exit_code;
    int exit_signal;
    int stop_requested;
    int log_pipe_fd[2];
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t *items;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    int exit_code;
    int exit_signal;
    exit_reason_t exit_reason;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
    size_t container_count;
} supervisor_ctx_t;

static supervisor_ctx_t g_ctx;

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s supervisor <base-rootfs>\n"
        "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s ps\n"
        "  %s logs <id>\n"
        "  %s stop <id>\n",
        prog, prog, prog, prog, prog, prog);
}

static const char *state_to_string(container_state_t state) {
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

static const char *reason_to_string(exit_reason_t reason) {
    switch (reason) {
    case EXIT_NORMAL:         return "normal";
    case EXIT_SIGNALED:        return "signaled";
    case EXIT_HARD_LIMIT:      return "hard_limit";
    case EXIT_STOP_REQUESTED:  return "stop_requested";
    default:                   return "unknown";
    }
}

static int parse_mib_flag(const char *flag, const char *value, unsigned long *target_bytes) {
    char *end = NULL;
    unsigned long mib;
    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc, char *argv[], int start_index) {
    for (int i = start_index; i < argc; i += 2) {
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
        } else if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
        } else if (strcmp(argv[i], "--nice") == 0) {
            char *end;
            long val = strtol(argv[i + 1], &end, 10);
            if (*end != '\0' || val < -20 || val > 19) {
                fprintf(stderr, "Invalid --nice value (expected -20..19): %s\n", argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)val;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        }
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static void ensure_log_directory(void) {
    struct stat st = {0};
    if (stat(LOG_DIR, &st) == -1) {
        mkdir(LOG_DIR, 0755);
    }
}

/* ============================================================================
 * Bounded Buffer Implementation
 * ============================================================================ */

static int bounded_buffer_init(bounded_buffer_t *buffer, size_t capacity) {
    memset(buffer, 0, sizeof(*buffer));
    buffer->capacity = capacity;
    buffer->items = calloc(capacity, sizeof(log_item_t));
    if (!buffer->items) return ENOMEM;
    
    int rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0) { free(buffer->items); return rc; }
    
    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buffer->mutex); free(buffer->items); return rc; }
    
    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        free(buffer->items);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer) {
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
    free(buffer->items);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer) {
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

static int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item) {
    pthread_mutex_lock(&buffer->mutex);
    
    while (buffer->count == buffer->capacity && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }
    
    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    
    memcpy(&buffer->items[buffer->tail], item, sizeof(log_item_t));
    buffer->tail = (buffer->tail + 1) % buffer->capacity;
    buffer->count++;
    
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

static int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item) {
    pthread_mutex_lock(&buffer->mutex);
    
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }
    
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    
    memcpy(item, &buffer->items[buffer->head], sizeof(log_item_t));
    buffer->head = (buffer->head + 1) % buffer->capacity;
    buffer->count--;
    
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ============================================================================
 * Container Metadata Management
 * ============================================================================ */

static container_record_t *find_container_by_id(const char *id) {
    container_record_t *c;
    for (c = g_ctx.containers; c != NULL; c = c->next) {
        if (strcmp(c->id, id) == 0) return c;
    }
    return NULL;
}

static container_record_t *find_container_by_pid(pid_t pid) {
    container_record_t *c;
    for (c = g_ctx.containers; c != NULL; c = c->next) {
        if (c->host_pid == pid) return c;
    }
    return NULL;
}

static container_record_t *create_container_record(const char *id, const char *rootfs,
                                                    const char *command, unsigned long soft,
                                                    unsigned long hard, int nice_val) {
    (void)rootfs;
    (void)command;
    
    container_record_t *c = calloc(1, sizeof(container_record_t));
    if (!c) return NULL;
    
    strncpy(c->id, id, CONTAINER_ID_LEN - 1);
    c->state = CONTAINER_STARTING;
    c->exit_reason = EXIT_NORMAL;
    c->soft_limit_bytes = soft;
    c->hard_limit_bytes = hard;
    c->nice_value = nice_val;
    c->stop_requested = 0;
    c->started_at = time(NULL);
    c->log_pipe_fd[0] = -1;
    c->log_pipe_fd[1] = -1;
    
    snprintf(c->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, id);
    
    pthread_mutex_lock(&g_ctx.metadata_lock);
    c->next = g_ctx.containers;
    g_ctx.containers = c;
    g_ctx.container_count++;
    pthread_mutex_unlock(&g_ctx.metadata_lock);
    
    return c;
}

static void remove_container_record(container_record_t *c) {
    pthread_mutex_lock(&g_ctx.metadata_lock);
    
    if (g_ctx.containers == c) {
        g_ctx.containers = c->next;
    } else {
        container_record_t *prev;
        for (prev = g_ctx.containers; prev && prev->next != c; prev = prev->next);
        if (prev) prev->next = c->next;
    }
    g_ctx.container_count--;
    pthread_mutex_unlock(&g_ctx.metadata_lock);
    
    free(c);
}

/* ============================================================================
 * Kernel Monitor Integration
 * ============================================================================ */

static int register_with_monitor(const char *container_id, pid_t host_pid,
                                  unsigned long soft, unsigned long hard) {
    if (g_ctx.monitor_fd < 0) return -1;
    
    struct monitor_request req = {0};
    req.pid = host_pid;
    req.soft_limit_bytes = soft;
    req.hard_limit_bytes = hard;
    strncpy(req.container_id, container_id, MONITOR_NAME_LEN - 1);
    
    return ioctl(g_ctx.monitor_fd, MONITOR_REGISTER, &req);
}

static int unregister_from_monitor(const char *container_id, pid_t host_pid) {
    if (g_ctx.monitor_fd < 0) return -1;
    
    struct monitor_request req = {0};
    req.pid = host_pid;
    strncpy(req.container_id, container_id, MONITOR_NAME_LEN - 1);
    
    return ioctl(g_ctx.monitor_fd, MONITOR_UNREGISTER, &req);
}

/* ============================================================================
 * Logging Thread (Consumer)
 * ============================================================================ */

static void *logging_thread(void *arg) {
    (void)arg;
    log_item_t item;
    ssize_t written;
    
    while (bounded_buffer_pop(&g_ctx.log_buffer, &item) == 0) {
        container_record_t *c = find_container_by_id(item.container_id);
        if (c && c->log_path[0]) {
            int fd = open(c->log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) {
                written = write(fd, item.data, item.length);
                (void)written;
                close(fd);
            }
        }
    }
    
    /* Drain remaining items before exit */
    while (bounded_buffer_pop(&g_ctx.log_buffer, &item) == 0) {
        container_record_t *c = find_container_by_id(item.container_id);
        if (c && c->log_path[0]) {
            int fd = open(c->log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) {
                written = write(fd, item.data, item.length);
                (void)written;
                close(fd);
            }
        }
    }
    
    return NULL;
}

/* ============================================================================
 * Producer Thread (Reads from container pipes)
 * ============================================================================ */

static void *producer_thread(void *arg) {
    container_record_t *c = (container_record_t *)arg;
    char buffer[LOG_CHUNK_SIZE];
    ssize_t n;
    
    /* Close write end in producer */
    close(c->log_pipe_fd[1]);
    
    while ((n = read(c->log_pipe_fd[0], buffer, sizeof(buffer) - 1)) > 0) {
        log_item_t item = {0};
        strncpy(item.container_id, c->id, CONTAINER_ID_LEN - 1);
        item.length = n;
        memcpy(item.data, buffer, n);
        item.data[n] = '\0';
        
        if (bounded_buffer_push(&g_ctx.log_buffer, &item) != 0) {
            break;
        }
    }
    
    close(c->log_pipe_fd[0]);
    return NULL;
}

/* ============================================================================
 * Container Child Process
 * ============================================================================ */

struct child_args {
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int log_fd;
    int nice_value;
};

static int child_fn(void *arg) {
    struct child_args *args = (struct child_args *)arg;
    
    /* Set nice value */
    if (args->nice_value != 0) {
        nice(args->nice_value);
    }
    
    /* Unshare namespaces */
    if (unshare(CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS) == -1) {
        perror("unshare");
        return 1;
    }
    
    /* Mount /proc */
    if (mount("proc", "/proc", "proc", 0, NULL) == -1) {
        perror("mount /proc");
    }
    
    /* Chroot */
    if (chroot(args->rootfs) == -1) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") == -1) {
        perror("chdir");
        return 1;
    }
    
    /* Redirect stdout/stderr to log pipe */
    dup2(args->log_fd, STDOUT_FILENO);
    dup2(args->log_fd, STDERR_FILENO);
    if (args->log_fd != STDOUT_FILENO && args->log_fd != STDERR_FILENO) {
        close(args->log_fd);
    }
    
    /* Execute command */
    execl("/bin/sh", "sh", "-c", args->command, (char *)NULL);
    perror("execl");
    return 1;
}

static pid_t launch_container(container_record_t *c, const char *rootfs, const char *command) {
    struct child_args args;
    char *stack;
    char *stack_top;
    pid_t pid;
    
    strncpy(args.rootfs, rootfs, PATH_MAX - 1);
    strncpy(args.command, command, CHILD_COMMAND_LEN - 1);
    args.nice_value = c->nice_value;
    
    if (pipe(c->log_pipe_fd) == -1) {
        perror("pipe");
        return -1;
    }
    args.log_fd = c->log_pipe_fd[1];
    
    stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc stack");
        close(c->log_pipe_fd[0]);
        close(c->log_pipe_fd[1]);
        return -1;
    }
    stack_top = stack + STACK_SIZE;
    
    pid = clone(child_fn, stack_top,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                &args);
    
    free(stack);
    
    if (pid == -1) {
        perror("clone");
        close(c->log_pipe_fd[0]);
        close(c->log_pipe_fd[1]);
        return -1;
    }
    
    c->host_pid = pid;
    c->state = CONTAINER_RUNNING;
    
    /* Close write end in parent */
    close(c->log_pipe_fd[1]);
    c->log_pipe_fd[1] = -1;
    
    /* Start producer thread for this container */
    pthread_t producer;
    pthread_create(&producer, NULL, producer_thread, c);
    pthread_detach(producer);
    
    return pid;
}

/* ============================================================================
 * SIGCHLD Handler
 * ============================================================================ */

static void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_ctx.metadata_lock);
        container_record_t *c = find_container_by_pid(pid);
        if (c) {
            if (WIFEXITED(status)) {
                c->exit_code = WEXITSTATUS(status);
                c->state = CONTAINER_EXITED;
                if (c->stop_requested) {
                    c->exit_reason = EXIT_STOP_REQUESTED;
                } else {
                    c->exit_reason = EXIT_NORMAL;
                }
            } else if (WIFSIGNALED(status)) {
                c->exit_signal = WTERMSIG(status);
                c->state = CONTAINER_KILLED;
                if (c->stop_requested) {
                    c->exit_reason = EXIT_STOP_REQUESTED;
                } else if (c->exit_signal == SIGKILL) {
                    c->exit_reason = EXIT_HARD_LIMIT;
                } else {
                    c->exit_reason = EXIT_SIGNALED;
                }
            }
            
            /* Unregister from kernel monitor */
            unregister_from_monitor(c->id, c->host_pid);
            
            /* Close read pipe if still open */
            if (c->log_pipe_fd[0] != -1) {
                close(c->log_pipe_fd[0]);
                c->log_pipe_fd[0] = -1;
            }
        }
        pthread_mutex_unlock(&g_ctx.metadata_lock);
    }
}

/* ============================================================================
 * Signal Handler for Supervisor Shutdown
 * ============================================================================ */

static void supervisor_signal_handler(int sig) {
    (void)sig;
    g_ctx.should_stop = 1;
}

/* ============================================================================
 * Control Socket Command Processing
 * ============================================================================ */

static void handle_start_command(control_request_t *req, control_response_t *resp) {
    container_record_t *existing = find_container_by_id(req->container_id);
    if (existing && (existing->state == CONTAINER_RUNNING || existing->state == CONTAINER_STARTING)) {
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "Container %s already running", req->container_id);
        return;
    }
    
    container_record_t *c = create_container_record(req->container_id, req->rootfs,
                                                     req->command, req->soft_limit_bytes,
                                                     req->hard_limit_bytes, req->nice_value);
    if (!c) {
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "Failed to create container record");
        return;
    }
    
    pid_t pid = launch_container(c, req->rootfs, req->command);
    if (pid < 0) {
        remove_container_record(c);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "Failed to launch container");
        return;
    }
    
    if (register_with_monitor(req->container_id, pid, req->soft_limit_bytes, req->hard_limit_bytes) != 0) {
        fprintf(stderr, "Warning: Failed to register with kernel monitor\n");
    }
    
    resp->status = 0;
    snprintf(resp->message, CONTROL_MESSAGE_LEN, "Container %s started with PID %d", req->container_id, pid);
}

static void handle_run_command(control_request_t *req, control_response_t *resp) {
    handle_start_command(req, resp);
    if (resp->status != 0) return;
    
    container_record_t *c = find_container_by_id(req->container_id);
    if (!c) {
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "Container not found");
        return;
    }
    
    /* Wait for container to exit */
    int status;
    waitpid(c->host_pid, &status, 0);
    
    resp->status = 0;
    resp->exit_code = c->exit_code;
    resp->exit_signal = c->exit_signal;
    resp->exit_reason = c->exit_reason;
    snprintf(resp->message, CONTROL_MESSAGE_LEN, "Container %s exited", req->container_id);
}

static void handle_ps_command(control_request_t *req, control_response_t *resp) {
    (void)req;
    char *buf = resp->message;
    size_t remaining = CONTROL_MESSAGE_LEN;
    int written;
    
    pthread_mutex_lock(&g_ctx.metadata_lock);
    
    written = snprintf(buf, remaining,
        "%-12s %-8s %-12s %-10s %-12s %s\n",
        "ID", "PID", "STATE", "EXIT", "SOFT(MiB)", "HARD(MiB)");
    buf += written; remaining -= written;
    
    for (container_record_t *c = g_ctx.containers; c; c = c->next) {
        written = snprintf(buf, remaining,
            "%-12s %-8d %-12s %-10s %-12lu %lu\n",
            c->id, c->host_pid, state_to_string(c->state),
            (c->state == CONTAINER_EXITED || c->state == CONTAINER_KILLED) 
                ? reason_to_string(c->exit_reason) : "-",
            c->soft_limit_bytes >> 20, c->hard_limit_bytes >> 20);
        buf += written; remaining -= written;
    }
    
    pthread_mutex_unlock(&g_ctx.metadata_lock);
    
    resp->status = 0;
}

static void handle_logs_command(control_request_t *req, control_response_t *resp) {
    container_record_t *c = find_container_by_id(req->container_id);
    if (!c) {
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "Container %s not found", req->container_id);
        return;
    }
    
    snprintf(resp->message, CONTROL_MESSAGE_LEN, "Log file: %s", c->log_path);
    resp->status = 0;
}

static void handle_stop_command(control_request_t *req, control_response_t *resp) {
    container_record_t *c = find_container_by_id(req->container_id);
    if (!c) {
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "Container %s not found", req->container_id);
        return;
    }
    
    if (c->state != CONTAINER_RUNNING) {
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "Container %s not running", req->container_id);
        return;
    }
    
    c->stop_requested = 1;
    
    /* Try SIGTERM first */
    kill(c->host_pid, SIGTERM);
    
    /* Wait a bit, then SIGKILL if still running */
    usleep(100000);
    if (kill(c->host_pid, 0) == 0) {
        kill(c->host_pid, SIGKILL);
    }
    
    resp->status = 0;
    snprintf(resp->message, CONTROL_MESSAGE_LEN, "Container %s stopped", req->container_id);
}

static void process_control_request(int client_fd) {
    control_request_t req;
    control_response_t resp = {0};
    
    ssize_t n = recv(client_fd, &req, sizeof(req), 0);
    if (n != sizeof(req)) {
        resp.status = -1;
        snprintf(resp.message, CONTROL_MESSAGE_LEN, "Invalid request");
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }
    
    switch (req.kind) {
    case CMD_START: handle_start_command(&req, &resp); break;
    case CMD_RUN:   handle_run_command(&req, &resp); break;
    case CMD_PS:    handle_ps_command(&req, &resp); break;
    case CMD_LOGS:  handle_logs_command(&req, &resp); break;
    case CMD_STOP:  handle_stop_command(&req, &resp); break;
    default:
        resp.status = -1;
        snprintf(resp.message, CONTROL_MESSAGE_LEN, "Unknown command");
    }
    
    send(client_fd, &resp, sizeof(resp), 0);
}

/* ============================================================================
 * Supervisor Main Loop
 * ============================================================================ */

static int run_supervisor(const char *rootfs) {
    struct sockaddr_un addr;
    int rc;
    
    (void)rootfs;
    
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.server_fd = -1;
    g_ctx.monitor_fd = -1;
    
    /* Initialize metadata lock */
    rc = pthread_mutex_init(&g_ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }
    
    /* Initialize bounded buffer */
    rc = bounded_buffer_init(&g_ctx.log_buffer, LOG_BUFFER_CAPACITY);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&g_ctx.metadata_lock);
        return 1;
    }
    
    /* Ensure log directory exists */
    ensure_log_directory();
    
    /* Open kernel monitor device */
    g_ctx.monitor_fd = open(MONITOR_DEVICE, O_RDWR);
    if (g_ctx.monitor_fd < 0) {
        perror("open " MONITOR_DEVICE);
        fprintf(stderr, "Warning: Continuing without kernel monitor\n");
    }
    
    /* Create control socket */
    unlink(CONTROL_PATH);
    g_ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_ctx.server_fd < 0) {
        perror("socket");
        goto cleanup;
    }
    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    
    if (bind(g_ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto cleanup;
    }
    
    if (listen(g_ctx.server_fd, 10) < 0) {
        perror("listen");
        goto cleanup;
    }
    
    /* Install signal handlers */
    signal(SIGCHLD, sigchld_handler);
    signal(SIGINT, supervisor_signal_handler);
    signal(SIGTERM, supervisor_signal_handler);
    
    /* Start logging thread */
    pthread_create(&g_ctx.logger_thread, NULL, logging_thread, NULL);
    
    printf("Supervisor started. Control socket: %s\n", CONTROL_PATH);
    printf("Log directory: %s\n", LOG_DIR);
    
    /* Main event loop */
    while (!g_ctx.should_stop) {
        fd_set readfds;
        struct timeval tv = {1, 0};
        
        FD_ZERO(&readfds);
        FD_SET(g_ctx.server_fd, &readfds);
        
        int ready = select(g_ctx.server_fd + 1, &readfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        
        if (ready > 0 && FD_ISSET(g_ctx.server_fd, &readfds)) {
            int client_fd = accept(g_ctx.server_fd, NULL, NULL);
            if (client_fd >= 0) {
                process_control_request(client_fd);
                close(client_fd);
            }
        }
    }
    
    printf("Supervisor shutting down...\n");
    
    /* Stop all running containers */
    pthread_mutex_lock(&g_ctx.metadata_lock);
    for (container_record_t *c = g_ctx.containers; c; c = c->next) {
        if (c->state == CONTAINER_RUNNING) {
            c->stop_requested = 1;
            kill(c->host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&g_ctx.metadata_lock);
    
    /* Wait for containers to exit */
    usleep(500000);
    
    pthread_mutex_lock(&g_ctx.metadata_lock);
    for (container_record_t *c = g_ctx.containers; c; c = c->next) {
        if (c->state == CONTAINER_RUNNING) {
            kill(c->host_pid, SIGKILL);
        }
    }
    pthread_mutex_unlock(&g_ctx.metadata_lock);
    
    /* Wait for all children */
    while (waitpid(-1, NULL, 0) > 0);
    
cleanup:
    bounded_buffer_begin_shutdown(&g_ctx.log_buffer);
    pthread_join(g_ctx.logger_thread, NULL);
    
    if (g_ctx.server_fd >= 0) {
        close(g_ctx.server_fd);
        unlink(CONTROL_PATH);
    }
    if (g_ctx.monitor_fd >= 0) close(g_ctx.monitor_fd);
    
    /* Free container records */
    while (g_ctx.containers) {
        container_record_t *c = g_ctx.containers;
        g_ctx.containers = c->next;
        free(c);
    }
    
    bounded_buffer_destroy(&g_ctx.log_buffer);
    pthread_mutex_destroy(&g_ctx.metadata_lock);
    
    return 0;
}

/* ============================================================================
 * Client Functions
 * ============================================================================ */

static int send_control_request(const control_request_t *req) {
    struct sockaddr_un addr;
    control_response_t resp;
    int fd;
    
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }
    
    if (send(fd, req, sizeof(*req), 0) != sizeof(*req)) {
        perror("send");
        close(fd);
        return 1;
    }
    
    if (recv(fd, &resp, sizeof(resp), 0) != sizeof(resp)) {
        perror("recv");
        close(fd);
        return 1;
    }
    
    close(fd);
    
    if (resp.status != 0) {
        fprintf(stderr, "Error: %s\n", resp.message);
        return 1;
    }
    
    printf("%s\n", resp.message);
    return 0;
}

/* ============================================================================
 * CLI Command Handlers
 * ============================================================================ */

static int cmd_start(int argc, char *argv[]) {
    control_request_t req = {0};
    
    if (argc < 5) {
        fprintf(stderr, "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs, argv[3], PATH_MAX - 1);
    strncpy(req.command, argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    req.nice_value = 0;
    
    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;
    
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[]) {
    control_request_t req = {0};
    
    if (argc < 5) {
        fprintf(stderr, "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs, argv[3], PATH_MAX - 1);
    strncpy(req.command, argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    req.nice_value = 0;
    
    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;
    
    return send_control_request(&req);
}

static int cmd_ps(void) {
    control_request_t req = {0};
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[]) {
    control_request_t req = {0};
    
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[]) {
    control_request_t req = {0};
    
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

/* ============================================================================
 * Main Entry Point
 * ============================================================================ */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    
    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run") == 0)   return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps") == 0)    return cmd_ps();
    if (strcmp(argv[1], "logs") == 0)  return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop") == 0)  return cmd_stop(argc, argv);
    
    usage(argv[0]);
    return 1;
}
