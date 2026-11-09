#include "mapper.h"
#include "mr_internal.h"
#include "queue.h"
#include "readn_writen.h"
#include "log.h"
#include "macros.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <threads.h>
#include <unistd.h>

typedef struct {
        queue_t *queue;
        mr_t mr;
        int worker_id;
        mtx_t* write_mtx;
        size_t* pairs_count;
} mapper_worker_arg_t;

static int mapper_reader_main(void *arg);
static int mapper_worker_main(void *arg);
static int mapper_emit_pair(const char *token, const void *value, size_t value_len, void *emit_arg);

/* Processo mapper:
 * stdin  -> righe serializzate dal main
 * worker -> mapper utente
 * stdout -> coppie serializzate verso reducer
 */
int mapper_process_main(mr_t mr){
    size_t pairs_count = 0;
    int reader_result = 0;
    int worker_result = 0;
    int mtx_created = 0;
    int reader_created = 0;
    size_t n_threads = (mr->attr).mapper_threads;
    size_t created_workers = 0;
    queue_t* queue = NULL;
    thrd_t reader;
    thrd_t* worker = NULL;
    mapper_worker_arg_t* args = NULL;
    mtx_t write_mtx;
    size_t joined_workers = 0;

    /* Crea coda interna */
    CHECK_NULL(queue = queue_init((mr->attr).queue_size), "mapper_process_main: queue_init fallita");

    /* Crea mutex per sdout*/
    CHECK_THRD_SUCCESS(mtx_init(&write_mtx, mtx_plain), "mapper_process_main: mtx_init fallita");
    mtx_created = 1;

    CHECK_NULL(worker = malloc(n_threads * sizeof(thrd_t)), "mapper_process_main: malloc worker fallita");
    CHECK_NULL(args = malloc(n_threads * sizeof(*args)), "mapper_process_main: malloc args fallita");

    /* Thread reader:
    * legge da stdin
    * inserisce righe nella coda
    */
    CHECK_THRD_SUCCESS(thrd_create(&reader, mapper_reader_main, queue), "mapper_process_main: thrd_create reader fallita");
    reader_created = 1;

    /* Thread worker:
    * estraggono righe
    * chiamano mapper utente
    * emettono coppie
    */
    for(size_t i = 0; i < n_threads; i++){
        args[i].queue = queue;
        args[i].mr = mr;
        args[i].worker_id = (int)i;
        args[i].write_mtx = &write_mtx;
        args[i].pairs_count = &pairs_count;

        CHECK_THRD_SUCCESS(thrd_create(&worker[i], mapper_worker_main, &args[i]), "mapper_process_main: thrd_create worker fallita");

        created_workers++;
    }

    CHECK_THRD_SUCCESS(thrd_join(reader, &reader_result), "mapper_process_main: thrd_join reader fallita");
    reader_created = 0;

    CHECK_MINUS_ONE(reader_result, "mapper_process_main: reader terminato con errore");

    for(size_t i = 0; i < created_workers; i++){
        CHECK_THRD_SUCCESS(thrd_join(worker[i], &worker_result), "mapper_process_main: thrd_join worker fallita");

        joined_workers++;

        CHECK_MINUS_ONE(worker_result, "mapper_process_main: worker terminato con errore");
    }
    created_workers = 0;

    CHECK_MINUS_ONE(log_write("PAIRS_PRODUCED", "coppie prodotte dal mapper=%zu", pairs_count), "log_write");

    free(worker);
    free(args);
    queue_destroy(queue);
    mtx_destroy(&write_mtx);
    return 0;

fail:
    if(queue != NULL) queue_close(queue);

    if (reader_created) {
        thrd_join(reader, NULL);
    }

    for(size_t i = joined_workers; i < created_workers; i++){
        thrd_join(worker[i], NULL);
    }

    free(worker);
    free(args);

    if (queue != NULL) {
        queue_destroy(queue);
    }

    if (mtx_created) {
        mtx_destroy(&write_mtx);
    }

    log_write("ERRORE", "errore mapper_process_main");
    return -1;
}


/* Thread reader del mapper:
 * legge righe serializzate da stdin
 * ricostruisce mr_file_line_t
 * inserisce nella coda dei worker
 */
static int mapper_reader_main(void* arg){
    mr_line_header_t header;
    size_t file_name_len = 0;
    char* file_name = NULL;
    size_t line_len = 0;
    char* line = NULL;
    mr_file_line_t* mr_file_line = NULL;
    queue_t* queue = (queue_t*)arg;

    CHECK_MINUS_ONE(log_write("THREAD_START", "mapper reader avviato"), "log_write");

    /* Record riga:
    * lunghezza nome | nome | numero riga | lunghezza riga | riga
    */
    while(1){
        
        /* Lettura header riga. */
        CHECK_READ_FULL_OR_EOF(readn(STDIN_FILENO, &header, sizeof(header)), sizeof(header), "mapper_reader_main: lettura header riga fallita");
        if(header.file_name_len <= 0){
            log_write("ERRORE", "mapper_reader_main: file_name_len non valido");
            goto fail;
        }
        if(header.line_len < 0){
            log_write("ERRORE", "mapper_reader_main: line_len non valido");
            goto fail;
        }
        file_name_len = (size_t)header.file_name_len;
        line_len = (size_t)header.line_len;

        /* Lettura nome file */
        CHECK_NULL(file_name = malloc(file_name_len * sizeof(char)), "mapper_reader_main: malloc file_name fallita");
        CHECK_READ_WRITE_FULL(readn(STDIN_FILENO, file_name, file_name_len), file_name_len, "mapper_reader_main: lettura file_name fallita");

        /* Lettura riga */
        if(line_len != 0){
            CHECK_NULL(line = malloc(line_len * sizeof(char)), "mapper_reader_main: malloc line fallita");
            CHECK_READ_WRITE_FULL(readn(STDIN_FILENO, line, line_len), line_len, "mapper_reader_main: lettura line fallita");
        }else{
            line = NULL;
        }

        /* Ricostruzione struttura pubblica vista dal mapper utente. */
        CHECK_NULL(mr_file_line = malloc(sizeof(mr_file_line_t)), "mapper_reader_main: malloc mr_file_line fallita");
        mr_file_line->file_name = file_name;
        mr_file_line->file_name_len = file_name_len;
        mr_file_line->line_number = header.line_number;
        mr_file_line->line = line;
        mr_file_line->line_len = line_len;

        /* Passaggio della riga ai worker. */
        CHECK_MINUS_ONE(queue_push(queue, mr_file_line), "mapper_reader_main: queue_push fallita");

        file_name = NULL;
        line = NULL;
        mr_file_line = NULL;
    }

eof:
    CHECK_MINUS_ONE(queue_close(queue), "mapper_reader_main: queue_close fallita");
    CHECK_MINUS_ONE(log_write("THREAD_END", "mapper reader terminato"), "log_write");
    return 0;

fail:
    free(file_name);
    free(line);
    free(mr_file_line);
    queue_close(queue);
    log_write("ERRORE", "mapper reader terminato con errore");
    return -1;
}

/* Thread worker del mapper:
 * estrae righe dalla coda
 * invoca mapper utente
 * libera la riga elaborata
 */
static int mapper_worker_main(void* arg){
    mapper_worker_arg_t* mapper_worker_arg = (mapper_worker_arg_t*)arg;
    mr_file_line_t* mr_file_line = NULL;

    CHECK_MINUS_ONE(log_write("THREAD_START", "mapper worker %d avviato", mapper_worker_arg->worker_id), "log_write");

    while((mr_file_line = (mr_file_line_t*)queue_pop(mapper_worker_arg->queue)) != NULL){
        /* Invoca il mapper utente sulla riga corrente.
        * Le coppie vengono emesse tramite mapper_emit_pair().
        */
        int res = mapper_worker_arg->mr->mapper(mr_file_line, mapper_emit_pair, mapper_worker_arg, mapper_worker_arg->mr->user_arg);
        if(res == -1){
            log_write("ERRORE", "mapper worker %d terminato con errore", mapper_worker_arg->worker_id);
            goto fail;
        }

        free((char *)mr_file_line->file_name);
        free((char *)mr_file_line->line);
        free(mr_file_line);
    }

    CHECK_MINUS_ONE(log_write("THREAD_END", "mapper worker %d terminato", mapper_worker_arg->worker_id), "log_write");
    return 0;

fail:
    queue_close(mapper_worker_arg->queue);
    if(mr_file_line != NULL){
        free((char *)mr_file_line->file_name);
        free((char *)mr_file_line->line);
        free(mr_file_line);
    }

    return -1;
}

/* Funzione usata dal mapper utente per emettere una coppia <token, valore>. */
static int mapper_emit_pair(const char* token, const void *value, size_t value_len,void *emit_arg){
    size_t token_len = strlen(token);
    mr_pair_header_t mr_pair_header = {.token_len = (int)token_len, .value_len = (int)value_len};   
    mapper_worker_arg_t *mapper_arg = emit_arg;
    int locked = 0;

    CHECK_THRD_SUCCESS(mtx_lock(mapper_arg->write_mtx), "mapper_emit_pair: mtx_lock fallita");
    locked = 1;

    /* Header coppia:
    * lunghezza token
    * lunghezza valore opaco
    */
    CHECK_READ_WRITE_FULL(writen(STDOUT_FILENO, &mr_pair_header, sizeof(mr_pair_header_t)), sizeof(mr_pair_header_t), "mapper_emit_pair: scrittura header fallita");

    /* Token */
    CHECK_READ_WRITE_FULL(writen(STDOUT_FILENO, token, token_len), token_len, "mapper_emit_pair: scrittura token fallita");

    /* Value */
    CHECK_READ_WRITE_FULL(writen(STDOUT_FILENO, value, value_len), value_len, "mapper_emit_pair: scrittura value fallita");

    (*mapper_arg->pairs_count)++;

    locked = 0;
    CHECK_THRD_SUCCESS(mtx_unlock(mapper_arg->write_mtx), "mapper_emit_pair: mtx_unlock fallita");
    return 0;

fail:
    if(locked) mtx_unlock(mapper_arg->write_mtx);
    log_write("ERRORE", "mapper_emit_pair: errore emissione coppia");
    return -1;
}

