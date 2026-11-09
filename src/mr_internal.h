#ifndef MR_INTERNAL_H
#define MR_INTERNAL_H
#include "../include/mr.h"

struct mr{
    mr_mapper_t  mapper;
    mr_reducer_t reducer;
    void        *user_arg;
    mr_attr_t attr;
};


typedef struct {
    int file_name_len;
    unsigned long line_number;
    int line_len;
} mr_line_header_t;

typedef struct {
    int token_len;
    int value_len;
} mr_pair_header_t;

typedef struct {
    int token_len;
    int result_len;
} mr_result_header_t;


#endif