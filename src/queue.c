#include <threads.h>
#include <stdlib.h>

#include "queue.h"
#include "log.h"
#include "macros.h"

struct queue {
    void   **buf;
    size_t   capacity;
    size_t   head;
    size_t   tail;
    size_t   size;
    int      closed;
    mtx_t    mutex;
    cnd_t    not_full;
    cnd_t    not_empty;
};


/* 
Crea una coda circolare thread-safe con capacità massima capacity.
Ritorna NULL se capacity è 0 o se l'inizializzazione fallisce.
*/
queue_t* queue_init(size_t capacity) {
    queue_t* q = NULL;
    int mutex_ok = 0;
    int not_full_ok = 0;
    int not_empty_ok = 0;

    if (capacity == 0) return NULL;

    q = malloc(sizeof(*q));
    CHECK_NULL(q, "queue_init: malloc queue fallita");

    q->buf = malloc(capacity * sizeof(void *));
    CHECK_NULL(q->buf, "queue_init: malloc buffer fallita");

    q->capacity = capacity;
    q->head = 0;
    q->tail = 0;
    q->size = 0;
    q->closed = 0;

    CHECK_THRD_SUCCESS(mtx_init(&q->mutex, mtx_plain), "queue_init: mtx_init fallita");
    mutex_ok = 1;
    CHECK_THRD_SUCCESS(cnd_init(&q->not_full), "queue_init: cnd_init not_full fallita");
    not_full_ok = 1;
    CHECK_THRD_SUCCESS(cnd_init(&q->not_empty), "queue_init: cnd_init not_empty fallita");
    not_empty_ok = 1;

    return q;

fail:
    if(not_empty_ok) cnd_destroy(&q->not_empty);
    if(not_full_ok) cnd_destroy(&q->not_full);
    if(mutex_ok) mtx_destroy(&q->mutex);
    if(q != NULL) free(q->buf);
    free(q);
    log_write("ERRORE", "errore queue_init");
    return NULL;
}

/* 
Estrae e ritorna un elemento dalla coda.
Se la coda è vuota attende; se è chiusa e vuota ritorna NULL. 
*/
void* queue_pop(queue_t *q){
    void* popped = NULL;

    CHECK_THRD_SUCCESS(mtx_lock(&q->mutex), "queue_pop: mtx_lock fallita");

    while(q->size == 0){
        if (q->closed) {
            CHECK_THRD_SUCCESS(mtx_unlock(&q->mutex), "queue_pop: mtx_unlock fallita");
            return NULL;
        }
        CHECK_THRD_SUCCESS(cnd_wait(&q->not_empty, &q->mutex), "queue_pop: cnd_wait fallita");
    }

    popped = q->buf[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->size--;

    CHECK_THRD_SUCCESS(cnd_signal(&q->not_full), "queue_pop: cnd_signal fallita");
    CHECK_THRD_SUCCESS(mtx_unlock(&q->mutex), "queue_pop: mtx_unlock fallita");

    return popped;

fail:
    return NULL;
}

/* 
Inserisce item nella coda. 
Se la coda è piena attende; se la coda è chiusa ritorna -1. 
*/
int queue_push(queue_t *q, void *item){
    CHECK_THRD_SUCCESS(mtx_lock(&q->mutex), "queue_push: mtx_lock fallita");

    while(q->size == q->capacity && !q->closed){
        CHECK_THRD_SUCCESS(cnd_wait(&q->not_full, &q->mutex), "queue_push: cnd_wait fallita");
    }

    if (q->closed) {
        CHECK_THRD_SUCCESS(mtx_unlock(&q->mutex), "queue_push: mtx_unlock fallita");
        return -1;
    }

    (q->buf)[q->tail] = item;
    q->tail = (q->tail+1) % q->capacity;
    q->size++;

    CHECK_THRD_SUCCESS(cnd_signal(&q->not_empty), "queue_push: cnd_signal fallita");
    CHECK_THRD_SUCCESS(mtx_unlock(&q->mutex), "queue_push: mtx_unlock fallita");

    return 0;

fail:
    return -1;
}

/* Chiude la coda e risveglia tutti i thread eventualmente bloccati in push o pop. */
int queue_close(queue_t *q) {
    int locked = 0;

    CHECK_THRD_SUCCESS(mtx_lock(&q->mutex), "queue_close: mtx_lock fallita");
    locked = 1;
    q->closed = 1;
    CHECK_THRD_SUCCESS(cnd_broadcast(&q->not_empty), "queue_close: cnd_broadcast not_empty fallita");
    CHECK_THRD_SUCCESS(cnd_broadcast(&q->not_full), "queue_close: cnd_broadcast not_full fallita");
    locked = 0;
    CHECK_THRD_SUCCESS(mtx_unlock(&q->mutex), "queue_close: mtx_unlock fallita");
    return 0;

fail:
    if(locked) mtx_unlock(&q->mutex);
    return -1;
}

/* 
Distrugge la coda e libera le risorse interne. 
Va chiamata solo quando nessun thread sta più usando la coda. 
*/
void  queue_destroy(queue_t *q){
    free(q->buf);
    mtx_destroy(&q->mutex);
    cnd_destroy(&q->not_empty);
    cnd_destroy(&q->not_full);
    free(q);
}