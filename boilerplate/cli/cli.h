#ifndef CLI_H
#define CLI_H

#include "../engine.h"

int cmd_start(int argc, char *argv[]);
int cmd_run(int argc, char *argv[]);
int cmd_ps(void);
int cmd_logs(int argc, char *argv[]);
int cmd_stop(int argc, char *argv[]);

#endif
