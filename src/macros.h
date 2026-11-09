#ifndef MACROS_H
#define MACROS_H

#include <sys/types.h>
#include <threads.h>

#include "log.h"

#define CHECK_MINUS_ONE(call, message)                                 \
    do {                                                               \
        if ((call) == -1) {                                            \
            log_write("ERRORE", (message));                            \
            goto fail;                                                 \
        }                                                              \
    } while (0)

#define CHECK_NULL(ptr, message)                                       \
    do {                                                               \
        if ((ptr) == NULL) {                                           \
            log_write("ERRORE", (message));                            \
            goto fail;                                                 \
        }                                                              \
    } while (0)

#define CHECK_READ_FULL_OR_EOF(call, expected, message)                \
    do {                                                               \
        ssize_t mr_check_rw_result = (call);                           \
        if (mr_check_rw_result == 0) {                                 \
            goto eof;                                                  \
        }                                                              \
        if (mr_check_rw_result != (ssize_t)(expected)) {               \
            log_write("ERRORE", (message));                            \
            goto fail;                                                 \
        }                                                              \
    } while (0)

#define CHECK_READ_WRITE_FULL(call, expected, message)                 \
    do {                                                               \
        ssize_t mr_check_rw_result = (call);                           \
        if (mr_check_rw_result != (ssize_t)(expected)) {               \
            log_write("ERRORE", (message));                            \
            goto fail;                                                 \
        }                                                              \
    } while (0)

#define CHECK_THRD_SUCCESS(call, message)                              \
    do {                                                               \
        if ((call) != thrd_success) {                                  \
            log_write("ERRORE", (message));                            \
            goto fail;                                                 \
        }                                                              \
    } while (0)

#endif
