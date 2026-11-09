#include "../include/mr.h"
#include "mr_internal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <errno.h>

#include "mapper.h"
#include "reducer.h"
#include "input.h"
#include "output.h"
#include "macros.h"
#include "log.h"

static int close_if_open(int *fd);

/* Chiude un descrittore se è aperto */
static int close_if_open(int *fd)
{
    if (*fd == -1) {
        return 0;
    }

    if (close(*fd) == -1) {
        log_write("ERRORE", "close fallita");
        return -1;
    }

    *fd = -1;
    return 0;
}

/* Inizializza gli attributi con i valori di default */
int mr_attr_init(mr_attr_t* attr){
    attr->mapper_threads = 4;
    attr->reducer_threads = 4;
    attr->queue_size = 64;
    attr->log_file = NULL;

    return 0;
}

/* Imposta il numero di thread mapper */
int mr_attr_set_mapper_threads(mr_attr_t *attr, size_t n){
    if(n < 1){
        errno = EINVAL;
        return -1;
    }

    attr->mapper_threads = n;
    return 0;
}

/* Imposta il numero di thread reducer */
int mr_attr_set_reducer_threads(mr_attr_t *attr, size_t n){
    if(n < 1){
        errno = EINVAL;
        return -1;
    }

    attr->reducer_threads = n;
    return 0;
}

/* Imposta la dimensione delle code interne */
int mr_attr_set_queue_size(mr_attr_t *attr, size_t n){
    if(n < 1){
        errno = EINVAL;
        return -1;
    }

    attr->queue_size = n;
    return 0;
}

/* Imposta il percorso del file di log */
int mr_attr_set_log_file(mr_attr_t *attr, const char* path){
    size_t len = strlen(path) + 1;
    char* copy = malloc(len);

    if(copy == NULL){
        errno = ENOMEM;
        return -1;
    }

    memcpy(copy, path, len);
    free((char *)attr->log_file);
    attr->log_file = copy;

    return 0;
}

/* Libera le risorse associate agli attributi */
int mr_attr_destroy(mr_attr_t *attr){
    attr->mapper_threads = 0;
    attr->reducer_threads = 0;
    attr->queue_size = 0;

    free((char *)attr->log_file);
    attr->log_file = NULL;

    return 0;
}

/* Crea una nuova istanza del framework */
int mr_create(mr_t* mr,const mr_attr_t* attr,mr_mapper_t mapper,mr_reducer_t reducer,void *user_arg){
    *mr = malloc(sizeof(**mr));
    if(*mr == NULL) return -1;

    (*mr)->attr = *attr;
    (*mr)->mapper = mapper;
    (*mr)->reducer = reducer;
    (*mr)->user_arg = user_arg;

    return 0;
}

/* Avvia l'elaborazione MapReduce */
int mr_start(mr_t mr, const char *input_path, const char *output_path){
    if(log_init(mr->attr.log_file != NULL ? mr->attr.log_file : "mr.log") != 0) return -1;

    int main_to_mapper[2] = {-1, -1};
    int mapper_to_reducer[2] = {-1, -1};
    int reducer_to_main[2] = {-1, -1};

    CHECK_MINUS_ONE(pipe(main_to_mapper), "mr_start: pipe main_to_mapper");
    CHECK_MINUS_ONE(log_write("PIPE_CREATED", "creata pipe main_to_mapper read_fd=%d write_fd=%d", main_to_mapper[0], main_to_mapper[1]), "log_write");
    CHECK_MINUS_ONE(pipe(mapper_to_reducer), "mr_start: pipe mapper_to_reducer");
    CHECK_MINUS_ONE(log_write("PIPE_CREATED", "creata pipe mapper_to_reducer read_fd=%d write_fd=%d", mapper_to_reducer[0], mapper_to_reducer[1]), "log_write");
    CHECK_MINUS_ONE(pipe(reducer_to_main), "mr_start: reducer_to_main");
    CHECK_MINUS_ONE(log_write("PIPE_CREATED", "creata pipe reducer_to_main read_fd=%d write_fd=%d", reducer_to_main[0], reducer_to_main[1]), "log_write");
 
    pid_t pid1 = -1;
    pid_t pid2 = -1;

    pid1 = fork();
    CHECK_MINUS_ONE(pid1, "Errore fork mapper");
    if (pid1 > 0) CHECK_MINUS_ONE(log_write("PROCESS_CREATED", "processo mapper creato pid=%ld", (long)pid1), "log_write");
    if(pid1 == 0){
        CHECK_MINUS_ONE(dup2(main_to_mapper[0], STDIN_FILENO), "mr_start: dup 2");
        CHECK_MINUS_ONE(dup2(mapper_to_reducer[1], STDOUT_FILENO), "mr_start: dup 2");
        close_if_open(&main_to_mapper[0]);
        close_if_open(&main_to_mapper[1]);
        close_if_open(&mapper_to_reducer[0]);
        close_if_open(&mapper_to_reducer[1]);
        close_if_open(&reducer_to_main[0]);
        close_if_open(&reducer_to_main[1]);

        int rc = mapper_process_main(mr);
        exit(rc == 0 ? 0 : 1);
    }

    pid2 = fork();
    CHECK_MINUS_ONE(pid2, "Errore fork reducer");
    if (pid2 > 0) CHECK_MINUS_ONE(log_write("PROCESS_CREATED", "processo reducer creato pid=%ld", (long)pid2), "log_write");
    if(pid2 == 0){
        CHECK_MINUS_ONE(dup2(mapper_to_reducer[0], STDIN_FILENO), "mr_start: dup 2");
        CHECK_MINUS_ONE(dup2(reducer_to_main[1], STDOUT_FILENO), "mr_start: 2");
        close_if_open(&main_to_mapper[0]);
        close_if_open(&main_to_mapper[1]);
        close_if_open(&mapper_to_reducer[0]);
        close_if_open(&mapper_to_reducer[1]);
        close_if_open(&reducer_to_main[0]);
        close_if_open(&reducer_to_main[1]);

        int rc = reducer_process_main(mr);
        exit(rc == 0 ? 0 : 1);
    }

    close_if_open(&main_to_mapper[0]);
    close_if_open(&mapper_to_reducer[0]);
    close_if_open(&mapper_to_reducer[1]);
    close_if_open(&reducer_to_main[1]);

    CHECK_MINUS_ONE(input_send_lines(main_to_mapper[1], input_path), "mr_start: input_send_lines");
    close_if_open(&main_to_mapper[1]);
    
    CHECK_MINUS_ONE(output_write_lines(reducer_to_main[0], output_path), "mr_start: output_write_lines");
    close_if_open(&reducer_to_main[0]);

    int status1 = 0;
    int status2 = 0;
    CHECK_MINUS_ONE(waitpid(pid1, &status1, 0), "waitpid mapper fallita");
    CHECK_MINUS_ONE(waitpid(pid2, &status2, 0), "waitpid reducer fallita");
    if(!WIFEXITED(status1) || WEXITSTATUS(status1) != 0) goto fail;
    if(!WIFEXITED(status2) || WEXITSTATUS(status2) != 0) goto fail;

    CHECK_MINUS_ONE(log_close(), "log_close");
    return 0;

fail:
    close_if_open(&main_to_mapper[0]);
    close_if_open(&main_to_mapper[1]);
    close_if_open(&mapper_to_reducer[0]);
    close_if_open(&mapper_to_reducer[1]);
    close_if_open(&reducer_to_main[0]);
    close_if_open(&reducer_to_main[1]);
    if(pid1 > 0) while(waitpid(pid1, NULL, 0) == -1 && errno == EINTR);
    if(pid2 > 0) while(waitpid(pid2, NULL, 0) == -1 && errno == EINTR);
    log_close();
    return -1;
}

/* Distrugge un'istanza del framework */
int mr_destroy(mr_t mr)
{
    free(mr);
    return 0;
}