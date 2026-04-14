
#ifndef SUPERVISOR_H
#define SUPERVISOR_H

#include "../engine.h"

int run_supervisor(const char *rootfs);
int launch_container(supervisor_ctx_t *ctx, const control_request_t *req);
void handle_stop(supervisor_ctx_t *ctx, const char *id, control_response_t *resp);
void handle_ps(supervisor_ctx_t *ctx, control_response_t *resp);
void handle_logs(supervisor_ctx_t *ctx, const char *id, int client_fd);

#endif
