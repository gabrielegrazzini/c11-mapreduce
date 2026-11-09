#include "group_table.h"
#include "macros.h"
#include "log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static size_t hash_token(const char *token, size_t token_len);
static int token_cmp(const char *a, size_t a_len, const char *b, size_t b_len);
static mr_group_t *find_group(mr_group_t *head, const char *token, size_t token_len);
static mr_group_t *new_group(const char *token, size_t token_len, const void *value, size_t value_len);
static int append_byte_node(mr_byte_node_t **head, mr_byte_node_t **tail, size_t *count, const void *data, size_t len);
static int append_value(mr_group_t *group, const void *data, size_t len);
static void free_byte_list(mr_byte_node_t *head);
static int compare_group_ptrs(const void *a, const void *b);
static void free_bucket_list(mr_group_t *head);


/* Crea una tabella hash per raggruppare i token */
mr_group_table_t *mr_group_table_create(size_t buckets_count){
    mr_group_table_t *table = NULL;

    if(buckets_count == 0){
        log_write("ERRORE", "mr_group_table_create: buckets_count non valido");
        return NULL;
    }

    CHECK_NULL(table = malloc(sizeof(*table)), "mr_group_table_create: malloc tabella fallita");

    table->buckets = NULL;
    table->buckets_count = buckets_count;
    table->groups_count = 0;

    CHECK_NULL(table->buckets = calloc(buckets_count, sizeof(*table->buckets)), "mr_group_table_create: calloc buckets fallita");

    return table;

fail:
    free(table);
    return NULL;
}

/* Inserisce un valore nel gruppo associato al token */
int mr_group_table_insert(mr_group_table_t *table, const char *token, size_t token_len, const void *value, size_t value_len){
    size_t bucket = 0;
    mr_group_t *group = NULL;

    /* Genera la chiave del token */
    bucket = hash_token(token, token_len) % table->buckets_count;

    /* Cerca il gruppo nella lista di collisione del bucket. */
    group = find_group(table->buckets[bucket], token, token_len);

    /* Se trova il gruppo aggiunge value */
    if(group != NULL){
        return append_value(group, value, value_len);
    }

    /* Se non trova il gruppo ne crea uno nuovo e lo aggiunge in testa */
    CHECK_NULL(group = new_group(token, token_len, value, value_len), "mr_group_table_insert: new_group fallita");
    group->next = table->buckets[bucket];
    table->buckets[bucket] = group;
    table->groups_count++;

    return 0;

fail:
    return -1;
}

/* Converte la tabella in un array ordinato di gruppi */
int mr_group_table_to_sorted_array(mr_group_table_t *table, mr_group_t ***out_groups, size_t *out_count){
    mr_group_t **groups = NULL;
    size_t index = 0;
    *out_groups = NULL;
    *out_count = 0;

    if(table->groups_count == 0){
        return 0;
    }

    CHECK_NULL(groups = malloc(table->groups_count * sizeof(*groups)), "mr_group_table_to_sorted_array: malloc groups fallita");

    /* Copia tutti i gruppi dai bucket nell'array di gruppi */
    for(size_t i = 0; i < table->buckets_count; i++){
        mr_group_t *cur = table->buckets[i];

        while(cur){
            groups[index] = cur;
            index++;
            cur = cur->next;
        }
    }

    /* Ordina l'array di gruppi */
    qsort(groups, table->groups_count, sizeof(*groups), compare_group_ptrs);

    *out_groups = groups;
    *out_count = table->groups_count;
    return 0;

fail:
    free(groups);
    return -1;
}

/* Libera la memoria della hash table */
void mr_group_table_free(mr_group_table_t *table){
    if(table == NULL){
        return;
    }

    /* Libera ogni lista contenuta in ogni bucket */
    for(size_t i = 0; i < table->buckets_count; i++){
        free_bucket_list(table->buckets[i]);
    }

    free(table->buckets);
    free(table);
}

/* Calcola un hash deterministico sui byte del token. */
static size_t hash_token(const char *token, size_t token_len){
    size_t hash = 0;

    /* 
    Hash polinomiale: a ogni passo il valore precedente viene moltiplicato per 31
    ovvero un numero primo usato per dare peso all'ordine dei caratteri e ridurre collisioni semplici tra token simili. 
    */
    for(size_t i = 0; i < token_len; i++){
        hash = hash * 31 + (unsigned char)token[i];
    }

    return hash;
}

/* Confronta due token in ordine lessicografico usando lunghezze esplicite. */
static int token_cmp(const char *a, size_t a_len, const char *b, size_t b_len){
    size_t min_len = a_len < b_len ? a_len : b_len;
    int cmp = memcmp(a, b, min_len);

    if(cmp != 0) return cmp;
    if(a_len < b_len) return -1;
    if(a_len > b_len) return 1;
    return 0;
}

/* Cerca un gruppo con il token indicato nella lista di collisione di un bucket. */
static mr_group_t *find_group(mr_group_t *head, const char *token, size_t token_len){
    mr_group_t *cur = head;

    while(cur){
        if(cur->token_len == token_len && memcmp(cur->token, token, token_len) == 0){
            return cur;
        }

        cur = cur->next;
    }

    return NULL;
}

/* Crea un nuovo gruppo con token e primo valore intermedio. */
static mr_group_t *new_group(const char *token, size_t token_len, const void *value, size_t value_len){
    mr_group_t *group = NULL;

    if(token == NULL || token_len == 0){
        log_write("ERRORE", "new_group: token non valido");
        return NULL;
    }

    CHECK_NULL(group = calloc(1, sizeof(*group)), "new_group: calloc gruppo fallita");
    CHECK_NULL(group->token = malloc(token_len + 1), "new_group: malloc token fallita");

    memcpy(group->token, token, token_len);
    group->token[token_len] = '\0';
    group->token_len = token_len;

    CHECK_MINUS_ONE(append_value(group, value, value_len), "new_group: append_value fallita");

    return group;

fail:
    if(group){
        free(group->token);
        free_byte_list(group->values_head);
        free_byte_list(group->results_head);
        free(group);
    }
    return NULL;
}


/* Aggiunge un valore intermedio alla lista values di un gruppo. */
static int append_value(mr_group_t *group, const void *data, size_t len){
    return append_byte_node(&group->values_head, &group->values_tail, &group->values_count, data, len);
}

/* Aggiunge un risultato finale alla lista results di un gruppo. */
int mr_group_append_result(mr_group_t *group, const void *data, size_t len){
    if(group == NULL){
        log_write("ERRORE", "mr_group_append_result: group NULL");
        return -1;
    }

    return append_byte_node(&group->results_head, &group->results_tail, &group->results_count, data, len);
}

/* Aggiunge una copia di un buffer opaco in coda a una lista di nodi byte. */
static int append_byte_node(mr_byte_node_t **head, mr_byte_node_t **tail, size_t *count, const void *data, size_t len){
    mr_byte_node_t *node = NULL;

    if(len > 0 && data == NULL){
        log_write("ERRORE", "append_byte_node: data NULL con len > 0");
        return -1;
    }

    CHECK_NULL(node = malloc(sizeof(*node)), "append_byte_node: malloc nodo fallita");

    node->data = NULL;
    node->len = len;
    node->next = NULL;

    if(len > 0){
        CHECK_NULL(node->data = malloc(len), "append_byte_node: malloc dati fallita");
        memcpy(node->data, data, len);
    }

    if(*tail) (*tail)->next = node;
    else *head = node;

    *tail = node;
    (*count)++;
    return 0;

fail:
    if(node){
        free(node->data);
        free(node);
    }
    return -1;
}

/* Confronta due puntatori a gruppo per ordinare un array con qsort. */
static int compare_group_ptrs(const void *a, const void *b){
    const mr_group_t *group_a = *(const mr_group_t * const *)a;
    const mr_group_t *group_b = *(const mr_group_t * const *)b;

    return token_cmp(group_a->token, group_a->token_len, group_b->token, group_b->token_len);
}

/* Libera tutti i gruppi collegati nella lista di collisione di un bucket. */
static void free_bucket_list(mr_group_t *head){
    mr_group_t *next = NULL;

    while(head){
        next = head->next;

        free_byte_list(head->values_head);
        free_byte_list(head->results_head);
        free(head->token);
        free(head);

        head = next;
    }
}

/* Libera una lista di nodi byte e i dati opachi contenuti. */
static void free_byte_list(mr_byte_node_t *head){
    mr_byte_node_t *next = NULL;

    while(head){
        next = head->next;
        free(head->data);
        free(head);
        head = next;
    }
}
