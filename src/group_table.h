#ifndef GROUP_TABLE_H
#define GROUP_TABLE_H

#include <stddef.h>

typedef struct mr_byte_node {
    void *data;
    size_t len;
    struct mr_byte_node *next;
} mr_byte_node_t;

typedef struct mr_group {
    char *token;
    size_t token_len;
    mr_byte_node_t *values_head;
    mr_byte_node_t *values_tail;
    size_t values_count;
    mr_byte_node_t *results_head;
    mr_byte_node_t *results_tail;
    size_t results_count;
    struct mr_group *next;
} mr_group_t;

typedef struct {
    mr_group_t **buckets;
    size_t buckets_count;
    size_t groups_count;
} mr_group_table_t;

mr_group_table_t *mr_group_table_create(size_t buckets_count);
int mr_group_table_insert(mr_group_table_t *table, const char *token, size_t token_len, const void *value, size_t value_len);
int mr_group_table_to_sorted_array(mr_group_table_t *table, mr_group_t ***out_groups, size_t *out_count);
int mr_group_append_result(mr_group_t *group, const void *data, size_t len);
void mr_group_table_free(mr_group_table_t *table);

#endif