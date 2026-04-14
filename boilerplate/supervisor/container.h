#ifndef CONTAINER_H
#define CONTAINER_H

#include "../engine.h"

int launch_container(supervisor_ctx_t *ctx, const control_request_t *req);
container_record_t *find_container(supervisor_ctx_t *ctx, const char *id);
int register_with_monitor(int monitor_fd, const char *id, pid_t pid,
                          unsigned long soft, unsigned long hard);
int unregister_from_monitor(int monitor_fd, const char *id, pid_t pid);

#endif
