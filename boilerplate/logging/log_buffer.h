#ifndef LOG_BUFFER_H
#define LOG_BUFFER_H

#include "../engine.h"

int  bounded_buffer_init(bounded_buffer_t *buf);
void bounded_buffer_destroy(bounded_buffer_t *buf);
void bounded_buffer_begin_shutdown(bounded_buffer_t *buf);
int  bounded_buffer_push(bounded_buffer_t *buf, const log_item_t *item);
int  bounded_buffer_pop(bounded_buffer_t *buf, log_item_t *item);
void *logging_thread(void *arg);

#endif
