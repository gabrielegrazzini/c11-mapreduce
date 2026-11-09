#ifndef QUEUE_H
#define QUEUE_H

#include <stddef.h>
typedef struct queue queue_t;

queue_t* queue_init(size_t capacity);
void queue_destroy(queue_t *q);
int queue_push(queue_t *q, void *item);
void* queue_pop(queue_t *q);
int queue_close(queue_t *q);

#endif