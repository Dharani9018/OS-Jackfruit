/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE
#include "engine.h"
#include "cli/cli.h"
#include "supervisor/supervisor.h"

int main(int argc, char *argv[])
{
    if (argc < 2) 
    {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) 
    {
        if (argc < 3)
        {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)  return cmd_start(argc, argv);
    if (strcmp(argv[1], "run") == 0)    return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps") == 0)     return cmd_ps();
    if (strcmp(argv[1], "logs") == 0)   return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop") == 0)   return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
