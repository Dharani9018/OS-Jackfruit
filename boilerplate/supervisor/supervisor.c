#define _GNU_SOURCE
#include "supervisor.h"
#include "container.h"
#include "signal_handler.h"
#include "../logging/log_buffer.h"

static supervisor_ctx_t *g_ctx = NULL;
static void cleanup_exited_containers(supervisor_ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    
    container_record_t *prev = NULL;
    container_record_t *curr = ctx->containers;
    
    while (curr) {
        if (curr->state != CONTAINER_RUNNING && curr->state != CONTAINER_STARTING) {
            // Container is dead, remove it
            container_record_t *to_remove = curr;
            
            if (prev)
                prev->next = curr->next;
            else
                ctx->containers = curr->next;
            
            curr = curr->next;
            free(to_remove);
        } else {
            prev = curr;
            curr = curr->next;
        }
    }
    
    pthread_mutex_unlock(&ctx->metadata_lock);
}
static void reap_dead_containers(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;
    container_record_t *r;
    int changed = 0;

    // Non-blocking wait for any terminated child
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        
        r = ctx->containers;
        while (r) {
            if (r->host_pid == pid) {
                // Update container state
                if (r->stop_requested)
                    r->state = CONTAINER_STOPPED;
                else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)
                    r->state = CONTAINER_KILLED;
                else
                    r->state = CONTAINER_EXITED;
                
                // Store exit code
                if (WIFEXITED(status))
                    r->exit_code = WEXITSTATUS(status);
                else if (WIFSIGNALED(status))
                    r->exit_code = 128 + WTERMSIG(status);
                
                printf("Container %s (pid=%d) exited with state=%s, code=%d\n", 
                       r->id, pid, state_to_string(r->state), r->exit_code);
                
                changed = 1;
                break;
            }
            r = r->next;
        }
        
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

static int setup_socket(void)
{
    int fd;
    struct sockaddr_un addr;

    unlink(CONTROL_PATH);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 8) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

static void dispatch(supervisor_ctx_t *ctx, const control_request_t *req, int client_fd)
{
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    switch (req->kind) {
    case CMD_START:
    case CMD_RUN:
        if (launch_container(ctx, req) == 0) {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message), "OK started %s", req->container_id);
        } else {
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message), "ERR failed to start %s", req->container_id);
        }
        write(client_fd, &resp, sizeof(resp));
        if (req->kind == CMD_RUN) {
            container_record_t *r;
            while (1) {
                usleep(200000);
                pthread_mutex_lock(&ctx->metadata_lock);
                r = find_container(ctx, req->container_id);
                if (!r || r->state != CONTAINER_RUNNING) {
                    if (r) {
                        resp.status = r->exit_code;
                        snprintf(resp.message, sizeof(resp.message),
                            "EXITED %s code=%d", req->container_id, r->exit_code);
                    }
                    pthread_mutex_unlock(&ctx->metadata_lock);
                    write(client_fd, &resp, sizeof(resp));
                    break;
                }
                pthread_mutex_unlock(&ctx->metadata_lock);
            }
        }
        break;

    case CMD_STOP:
        handle_stop(ctx, req->container_id, &resp);
        write(client_fd, &resp, sizeof(resp));
        break;

    case CMD_PS:
        handle_ps(ctx, &resp);
        write(client_fd, &resp, sizeof(resp));
        break;

    case CMD_LOGS:
        handle_logs(ctx, req->container_id, client_fd);
        break;

    default:
        resp.status = 1;
        snprintf(resp.message, sizeof(resp.message), "ERR unknown command");
        write(client_fd, &resp, sizeof(resp));
        break;
    }
}

int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct timeval tv;
    fd_set readfds;
    int client_fd;
    control_request_t req;

    (void)rootfs;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;

    if (pthread_mutex_init(&ctx.metadata_lock, NULL) != 0) {
        perror("pthread_mutex_init");
        return 1;
    }

    if (bounded_buffer_init(&ctx.log_buffer) != 0) {
        perror("bounded_buffer_init");
        return 1;
    }

    mkdir(LOG_DIR, 0755);

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "Warning: could not open monitor device, memory limits disabled\n");

    ctx.server_fd = setup_socket();
    if (ctx.server_fd < 0)
        return 1;

    g_ctx = &ctx;
    install_signal_handlers(&ctx);

    if (pthread_create(&ctx.consumer_thread, NULL, logging_thread, &ctx.log_buffer) != 0) {
        perror("pthread_create consumer");
        return 1;
    }

    fprintf(stdout, "Supervisor ready on %s\n", CONTROL_PATH);
    fflush(stdout);

    while (!ctx.should_stop) {
        reap_dead_containers(&ctx);
        cleanup_exited_containers(&ctx);
        FD_ZERO(&readfds);
        FD_SET(ctx.server_fd, &readfds);
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        if (select(ctx.server_fd + 1, &readfds, NULL, NULL, &tv) <= 0)
            continue;

        client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0)
            continue;

        if (read(client_fd, &req, sizeof(req)) == sizeof(req))
            dispatch(&ctx, &req, client_fd);

        close(client_fd);
    }

    container_record_t *r = ctx.containers;
    while (r) {
        if (r->state == CONTAINER_RUNNING) {
            r->stop_requested = 1;
            kill(r->host_pid, SIGTERM);
        }
        r = r->next;
    }

    sleep(2);

    r = ctx.containers;
    while (r) {
        if (r->producer_thread)
            pthread_join(r->producer_thread, NULL);
        r = r->next;
    }

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.consumer_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    r = ctx.containers;
    while (r) {
        container_record_t *next = r->next;
        free(r);
        r = next;
    }

    if (ctx.monitor_fd >= 0)  close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);
    pthread_mutex_destroy(&ctx.metadata_lock);

    fprintf(stdout, "Supervisor exited cleanly.\n");
    return 0;
}
void handle_stop(supervisor_ctx_t *ctx, const char *id, control_response_t *resp)
{
    container_record_t *r;
    int waited = 0;

    pthread_mutex_lock(&ctx->metadata_lock);
    r = find_container(ctx, id);
    if (!r) {
        resp->status = 1;
        snprintf(resp->message, sizeof(resp->message), "ERR container %s not found", id);
        pthread_mutex_unlock(&ctx->metadata_lock);
        return;
    }
    
    // If already exited/stopped
    if (r->state != CONTAINER_RUNNING) {
        resp->status = 0;
        snprintf(resp->message, sizeof(resp->message), "OK container %s already stopped (state=%s)", 
                 id, state_to_string(r->state));
        pthread_mutex_unlock(&ctx->metadata_lock);
        return;
    }
    
    r->stop_requested = 1;
    kill(r->host_pid, SIGTERM);
    pthread_mutex_unlock(&ctx->metadata_lock);

    while (waited < STOP_TIMEOUT_SEC) {
        sleep(1);
        waited++;
        pthread_mutex_lock(&ctx->metadata_lock);
        r = find_container(ctx, id);
        if (!r || r->state != CONTAINER_RUNNING) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp->status = 0;
            snprintf(resp->message, sizeof(resp->message), "OK stopped %s", id);
            return;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    r = find_container(ctx, id);
    if (r && r->state == CONTAINER_RUNNING) {
        kill(r->host_pid, SIGKILL);
        r->state = CONTAINER_KILLED;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message), "OK force-killed %s", id);
}

void handle_ps(supervisor_ctx_t *ctx, control_response_t *resp)
{
    container_record_t *r;
    int offset = 0;

    pthread_mutex_lock(&ctx->metadata_lock);
    r = ctx->containers;
    offset += snprintf(resp->message + offset, sizeof(resp->message) - offset,
                       "%-12s %-8s %-10s %-8s\n", "ID", "PID", "STATE", "EXIT");
    while (r && offset < (int)sizeof(resp->message) - 1) {
        offset += snprintf(resp->message + offset, sizeof(resp->message) - offset,
                           "%-12s %-8d %-10s %-8d\n",
                           r->id, r->host_pid,
                           state_to_string(r->state),
                           r->exit_code);
        r = r->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
    resp->status = 0;
}

void handle_logs(supervisor_ctx_t *ctx, const char *id, int client_fd)
{
    container_record_t *r;
    char log_path[PATH_MAX];
    char buf[4096];
    int fd;
    ssize_t n;
    control_response_t resp;

    pthread_mutex_lock(&ctx->metadata_lock);
    r = find_container(ctx, id);
    if (r)
    { 
        strncpy(log_path, r->log_path, sizeof(log_path) - 1);
        log_path[sizeof(log_path) - 1] = '\0';
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!r) {
        memset(&resp, 0, sizeof(resp));
        resp.status = 1;
        snprintf(resp.message, sizeof(resp.message), "ERR container %s not found", id);
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    fd = open(log_path, O_RDONLY);
    if (fd < 0) {
        memset(&resp, 0, sizeof(resp));
        resp.status = 1;
        snprintf(resp.message, sizeof(resp.message), "ERR could not open log");
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(client_fd, buf, n);

    close(fd);
}
