#include "reducer.h"
#include "mr_internal.h"
#include "queue.h"
#include "readn_writen.h"
#include "group_table.h"
#include "macros.h"
#include "log.h"

#include <threads.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>

#define MR_MAX_TOKEN_LEN 4096
#define MR_MAX_VALUE_LEN (64 * 1024 * 1024)
#define BUCKET_COUNT 4096

typedef struct {
    queue_t *queue;
    mr_t mr;
    int worker_id;
} reducer_worker_arg_t;

static int groups_from_pairs(mr_group_table_t *table);
static int enqueue_all_groups(mr_group_t **groups, size_t groups_count, queue_t *queue);
static int reducer_worker_main(void *arg);
static int reducer_emit_result(const char *token, const void *result, size_t result_size, void *emit_arg);
static int write_group_results(mr_group_t *group);
static int write_all_group_results(mr_group_t **groups, size_t groups_count);
static size_t count_results(mr_group_t **groups, size_t groups_count);

/* 
Esegue il processo reducer: legge tutte le coppie dal mapper, raggruppa per token, 
avvia i thread reducer e infine scrive i risultati ordinati. 
*/
int reducer_process_main(mr_t mr){
    int worker_result = 0;
    size_t n_threads = (mr->attr).reducer_threads;
    size_t created_workers = 0;
    size_t joined_workers = 0;
    size_t groups_count = 0;
    queue_t* queue = NULL;
    mr_group_table_t *table = NULL;
    mr_group_t **groups = NULL;
    thrd_t* worker = NULL;
    reducer_worker_arg_t* args = NULL;

    /* Crea la tabella hash in cui raccogliere i gruppi */
    CHECK_NULL(table = mr_group_table_create(BUCKET_COUNT), "reducer_process_main: mr_group_table_create fallita");

    /* Prima raccoglie tutte le coppie ricevute dal mapper e costruisce i gruppi per token. */
    CHECK_MINUS_ONE(groups_from_pairs(table), "reducer_process_main: groups_from_pairs fallita");

    /* Ordina i gruppi in ordine lessicografico all'interno di un array */
    CHECK_MINUS_ONE(mr_group_table_to_sorted_array(table, &groups, &groups_count), "reducer_process_main: mr_group_table_to_sorted_array fallita");

    CHECK_MINUS_ONE(log_write("TOKENS_GROUPED", "token distinti raggruppati=%zu", groups_count), "log_write");
    
    /* Coda usata per distribuire i gruppi ai thread reducer. */
    CHECK_NULL(queue = queue_init((mr->attr).queue_size), "reducer_process_main: queue_init fallita");

    CHECK_NULL(worker = malloc(n_threads * sizeof(thrd_t)), "reducer_process_main: malloc worker fallita");
    CHECK_NULL(args = malloc(n_threads * sizeof(*args)), "reducer_process_main: malloc args fallita");

    /* 
    Crea i worker. 
    */
    for(size_t i = 0; i < n_threads; i++){
        args[i].queue = queue;
        args[i].mr = mr;
        args[i].worker_id = (int)i;
        CHECK_THRD_SUCCESS(thrd_create(&worker[i], reducer_worker_main, &args[i]), "reducer_process_main: thrd_create worker fallita");
        created_workers++;
    }

    /* Inserisce nella coda tutti i gruppi ordinati */
    CHECK_MINUS_ONE(enqueue_all_groups(groups, groups_count, queue), "reducer_process_main: enqueue_all_groups fallita");


    for(size_t i = 0; i < created_workers; i++){
        CHECK_THRD_SUCCESS(thrd_join(worker[i], &worker_result), "reducer_process_main: thrd_join worker fallita");
        joined_workers++;
        CHECK_MINUS_ONE(worker_result, "reducer_process_main: worker terminato con errore");
    }
    created_workers = 0;

    CHECK_MINUS_ONE(log_write("RESULTS_PRODUCED", "risultati finali prodotti=%zu", count_results(groups, groups_count)), "log_write");

    /* 
    Dopo la fine dei worker, i risultati vengono scritti in ordine lessicografico per token.
    */
    CHECK_MINUS_ONE(write_all_group_results(groups, groups_count), "reducer_process_main: scrittura risultati fallita");

    free(worker);
    free(args);
    free(groups);
    mr_group_table_free(table);
    queue_destroy(queue);
    return 0;

fail:
    if(queue != NULL) queue_close(queue);
    for(size_t i = joined_workers; i < created_workers; i++) thrd_join(worker[i], NULL);
    free(worker);
    free(args);
    free(groups);
    mr_group_table_free(table);
    if(queue != NULL) queue_destroy(queue);
    return -1;
}


/* Legge tutte le coppie ricevute dal mapper e le inserisce nella tabella hash. */
static int groups_from_pairs(mr_group_table_t *table){
    mr_pair_header_t header;
    char *token = NULL;
    void *value = NULL;

    /* Formato letto: header | token | valore opaco. */
    while(1){
        CHECK_READ_FULL_OR_EOF(readn(STDIN_FILENO, &header, sizeof(header)), sizeof(header), "groups_from_pairs: lettura header fallita");

        if(header.token_len <= 0 || header.value_len < 0) goto fail;
        if(header.token_len > MR_MAX_TOKEN_LEN || header.value_len > MR_MAX_VALUE_LEN) goto fail;

        CHECK_NULL(token = malloc((size_t)header.token_len + 1), "groups_from_pairs: malloc token fallita");

        CHECK_READ_WRITE_FULL(readn(STDIN_FILENO, token, (size_t)header.token_len), (size_t)header.token_len, "groups_from_pairs: lettura token fallita");

        token[header.token_len] = '\0';

        if(header.value_len > 0){
            CHECK_NULL(value = malloc((size_t)header.value_len), "groups_from_pairs: malloc value fallita");
            CHECK_READ_WRITE_FULL(readn(STDIN_FILENO, value, (size_t)header.value_len), (size_t)header.value_len, "groups_from_pairs: lettura value fallita");
        }

        CHECK_MINUS_ONE(mr_group_table_insert(table, token, (size_t)header.token_len, value, (size_t)header.value_len), "mr_group_table_insert: mr_group_inser fallita");

        free(token);
        free(value);
        token = NULL;
        value = NULL;
    }

eof:
    free(token);
    free(value);
    return 0;
fail:
    free(token);
    free(value);
    return -1;
}

/* Inserisce nella coda tutti i gruppi dell'array ordinato e poi chiude la coda. */
static int enqueue_all_groups(mr_group_t **groups, size_t groups_count, queue_t *queue){
    for(size_t i = 0; i < groups_count; i++){
        CHECK_MINUS_ONE(queue_push(queue, groups[i]), "enqueue_all_groups: queue_push fallita");
    }
    CHECK_MINUS_ONE(queue_close(queue), "enqueue_all_groups: queue_close fallita");
    return 0;

fail:
    queue_close(queue);
    return -1;
}

/* Thread worker del reducer: estrae gruppi dalla coda e invoca la reducer utente. */
static int reducer_worker_main(void *arg){
    reducer_worker_arg_t* worker_arg = arg;
    mr_group_t* group = NULL;
    mr_value_t* values = NULL;
    mr_byte_node_t* value_node = NULL;

    CHECK_MINUS_ONE(log_write("THREAD_START", "reducer worker %d avviato", worker_arg->worker_id), "log_write");

    while((group = queue_pop(worker_arg->queue)) != NULL){
        CHECK_NULL(values = malloc(group->values_count * sizeof(mr_value_t)), "reducer_worker_main: malloc values fallita");

        value_node = group->values_head;

        for(size_t i = 0; i < group->values_count; i++){
            values[i].data = value_node->data;
            values[i].size = value_node->len;
            value_node = value_node->next;
        }

        int res = worker_arg->mr->reducer(group->token, values, group->values_count, reducer_emit_result, group, worker_arg->mr->user_arg);
        CHECK_MINUS_ONE(res, "reducer_worker_main: reducer fallita");

        free(values);
        values = NULL;
    }

    CHECK_MINUS_ONE(log_write("THREAD_END", "reducer worker %d terminato", worker_arg->worker_id), "log_write");

    return 0;

fail:
    queue_close(worker_arg->queue);
    free(values);
    return -1;
}

/* Callback usata dalla reducer utente per salvare un risultato nel gruppo corrente. */
static int reducer_emit_result(const char *token, const void *result, size_t result_size, void *emit_arg){
    mr_group_t* group = emit_arg;

    (void)token;

    return mr_group_append_result(group, result, result_size);
}

/* Scrive sullo stdout del reducer tutti i risultati prodotti per un gruppo. */
static int write_group_results(mr_group_t *group){
    mr_byte_node_t* head = group->results_head;
    mr_result_header_t header;

    /* controlla che results sia <= a INT_MAX */
    if(group->token_len > INT_MAX) goto fail;

    header.token_len = (int)group->token_len;

    while(head){
        if(head->len > INT_MAX) goto fail;

        header.result_len = (int)head->len;

        /* Record pipe: header | token | risultato */
        CHECK_READ_WRITE_FULL(writen(STDOUT_FILENO, &header, sizeof(header)), sizeof(header), "write_group_results: scrittura header risultato fallita");
        CHECK_READ_WRITE_FULL(writen(STDOUT_FILENO, group->token, (size_t)header.token_len), (size_t)header.token_len, "write_group_results: scrittura token fallita");
        CHECK_READ_WRITE_FULL(writen(STDOUT_FILENO, head->data, (size_t)header.result_len), (size_t)header.result_len, "write_group_results: scrittura result fallita");

        head = head->next;
    }

    return 0;

fail:
    return -1;
}

/* Scrive tutti i gruppi seguendo l'ordine dell'array ordinato per token. */
static int write_all_group_results(mr_group_t **groups, size_t groups_count){
    for(size_t i = 0; i < groups_count; i++){
        CHECK_MINUS_ONE(write_group_results(groups[i]), "write_all_group_results: write_group_results fallita");
    }
    return 0;

fail:
    return -1;
}

/* Conta il numero totale di risultati prodotti da tutti i gruppi. */
static size_t count_results(mr_group_t **groups, size_t groups_count){
    size_t count = 0;

    for(size_t i = 0; i < groups_count; i++){
        count += groups[i]->results_count;
    }

    return count;
}