#define _GNU_SOURCE
#include "container.h"
#include "../logging/log_buffer.h"

extern bounded_buffer_t *g_log_buffer;

static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    if (chroot(cfg->rootfs) < 0) {
        perror("chroot");
        _exit(1);
    }
    if (chdir("/") < 0) {
        perror("chdir");
        _exit(1);
    }

    mount("proc", "/proc", "proc", 0, NULL);

    dup2(cfg->pipe_write_fd, STDOUT_FILENO);
    dup2(cfg->pipe_write_fd, STDERR_FILENO);
    close(cfg->pipe_write_fd);

    if (cfg->nice_value != 0)
        setpriority(PRIO_PROCESS, 0, cfg->nice_value);

    char *argv[] = { cfg->command, NULL };
    execve(cfg->command, argv, NULL);
    perror("execve");
    _exit(1);
}

static void *producer_thread(void *arg)
{
    producer_arg_t *parg = (producer_arg_t *)arg;
    char buf[LOG_CHUNK_SIZE];
    ssize_t n;
    log_item_t item;

    while ((n = read(parg->pipe_read_fd, buf, sizeof(buf))) > 0) {
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, parg->container_id, CONTAINER_ID_LEN - 1);
        item.container_id[CONTAINER_ID_LEN - 1] = '\0';
        item.length = (size_t)n;
        memcpy(item.data, buf, (size_t)n);
        
        // Debug: print what we're logging
        fprintf(stderr, "Producer: logging %zu bytes from container %s\n", 
                (size_t)n, parg->container_id);
        
        bounded_buffer_push(parg->log_buffer, &item);
    }

    fprintf(stderr, "Producer: container %s pipe closed, exiting\n", parg->container_id);
    close(parg->pipe_read_fd);
    free(parg);
    return NULL;
}
int launch_container(supervisor_ctx_t *ctx, const control_request_t *req)
{
    int pipefd[2];
    char *stack;
    pid_t pid;
    child_config_t cfg;
    container_record_t *record;
    producer_arg_t *parg;

pthread_mutex_lock(&ctx->metadata_lock);
container_record_t *existing = find_container(ctx, req->container_id);
if (existing != NULL) {
    // Remove the old entry first
    if (existing->state == CONTAINER_RUNNING) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        fprintf(stderr, "Container %s already running\n", req->container_id);
        return -1;
    }
    // Remove the exited container from list
    container_record_t *prev = NULL;
    container_record_t *curr = ctx->containers;
    while (curr) {
        if (strcmp(curr->id, req->container_id) == 0) {
            if (prev)
                prev->next = curr->next;
            else
                ctx->containers = curr->next;
            free(curr);
            break;
        }
        prev = curr;
        curr = curr->next;
    }
}
pthread_mutex_unlock(&ctx->metadata_lock);

    if (pipe(pipefd) < 0) {
        perror("pipe");
        return -1;
    }

    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.id,      req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg.rootfs,  req->rootfs,       PATH_MAX - 1);
    strncpy(cfg.command, req->command,      CHILD_COMMAND_LEN - 1);
    cfg.nice_value    = req->nice_value;
    cfg.pipe_write_fd = pipefd[1];

    stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc stack");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    pid = clone(child_fn, stack + STACK_SIZE,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                &cfg);

    free(stack);

    if (pid < 0) {
        perror("clone");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    close(pipefd[1]);

    record = calloc(1, sizeof(container_record_t));
    if (!record) {
        perror("calloc");
        close(pipefd[0]);
        return -1;
    }

    strncpy(record->id, req->container_id, CONTAINER_ID_LEN - 1);
    record->host_pid          = pid;
    record->started_at        = time(NULL);
    record->state             = CONTAINER_RUNNING;
    record->soft_limit_bytes  = req->soft_limit_bytes;
    record->hard_limit_bytes  = req->hard_limit_bytes;
    record->pipe_read_fd      = pipefd[0];
    snprintf(record->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);

    parg = malloc(sizeof(producer_arg_t));
    parg->pipe_read_fd = pipefd[0];
    strncpy(parg->container_id, req->container_id, CONTAINER_ID_LEN - 1);
    parg->log_buffer = &ctx->log_buffer;

    pthread_create(&record->producer_thread, NULL, producer_thread, parg);

    pthread_mutex_lock(&ctx->metadata_lock);
    record->next   = ctx->containers;
    ctx->containers = record;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                              req->soft_limit_bytes, req->hard_limit_bytes);

    fprintf(stdout, "Started container %s pid=%d\n", req->container_id, pid);
    return 0;
}

container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *r = ctx->containers;
    while (r) {
        if (strcmp(r->id, id) == 0)
            return r;
        r = r->next;
    }
    return NULL;
}

int register_with_monitor(int monitor_fd, const char *id, pid_t pid,
                          unsigned long soft, unsigned long hard)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid               = pid;
    req.soft_limit_bytes  = soft;
    req.hard_limit_bytes  = hard;
    strncpy(req.container_id, id, CONTAINER_ID_LEN - 1);
    return ioctl(monitor_fd, MONITOR_REGISTER, &req);
}

int unregister_from_monitor(int monitor_fd, const char *id, pid_t pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = pid;
    strncpy(req.container_id, id, CONTAINER_ID_LEN - 1);
    return ioctl(monitor_fd, MONITOR_UNREGISTER, &req);
}
