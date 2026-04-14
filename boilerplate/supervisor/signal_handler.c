#define _GNU_SOURCE
#include "signal_handler.h"
#include "container.h"

static supervisor_ctx_t *g_ctx = NULL;

static void sigchld_handler(int sig)
{
    int status;
    pid_t pid;
    container_record_t *r;

    (void)sig;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (!g_ctx) continue;

        r = g_ctx->containers;
        while (r) {
            if (r->host_pid == pid) {
                if (WIFEXITED(status))
                    r->exit_code = WEXITSTATUS(status);
                if (WIFSIGNALED(status))
                    r->exit_signal = WTERMSIG(status);

                if (r->stop_requested)
                    r->state = CONTAINER_STOPPED;
                else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)
                    r->state = CONTAINER_KILLED;
                else
                    r->state = CONTAINER_EXITED;

                break;
            }
            r = r->next;
        }
    }
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->should_stop = 1;
}

void install_signal_handlers(supervisor_ctx_t *ctx)
{
    struct sigaction sa;

    g_ctx = ctx;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
}
