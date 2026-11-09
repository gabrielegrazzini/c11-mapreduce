#include "mr.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int length_mapper(const mr_file_line_t *line, mr_emit_pair_t emit, void *emit_arg, void *user_arg) {
    (void)user_arg;

    size_t i = 0;

    while (i < line->line_len) {
        while (i < line->line_len && !isalnum((unsigned char)line->line[i])) i++;

        size_t start = i;

        while (i < line->line_len && isalnum((unsigned char)line->line[i])) i++;

        size_t len = i - start;

        if (len > 0) {
            char token[32];
            int written = snprintf(token, sizeof(token), "len%zu", len);
            if (written < 0 || written >= (int)sizeof(token)) return -1;

            int one = 1;
            if (emit(token, &one, sizeof(one), emit_arg) != 0) return -1;
        }
    }

    return 0;
}

static int length_reducer(const char *token, const mr_value_t *values, size_t values_count, mr_emit_result_t emit, void *emit_arg, void *user_arg) {
    (void)user_arg;

    int total = 0;

    for (size_t i = 0; i < values_count; i++) {
        int value;

        if (values[i].size != sizeof(int)) return -1;

        memcpy(&value, values[i].data, sizeof(value));
        total += value;
    }

    return emit(token, &total, sizeof(total), emit_arg);
}

static void print_output_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        perror("fopen output");
        return;
    }

    while (1) {
        int token_len;
        int result_len;

        if (fread(&token_len, sizeof(token_len), 1, fp) != 1) break;

        char *token = malloc((size_t)token_len + 1);
        if (token == NULL) break;

        fread(token, 1, (size_t)token_len, fp);
        token[token_len] = '\0';

        fread(&result_len, sizeof(result_len), 1, fp);

        int total = 0;
        if (result_len == (int)sizeof(int)) fread(&total, sizeof(total), 1, fp);
        else fseek(fp, result_len, SEEK_CUR);

        printf("%s %d\n", token, total);
        free(token);
    }

    fclose(fp);
}

int main(void) {
    const char *input_path = "examples/word_length/input";
    const char *output_path = "examples/word_length/output.mro";

    mr_attr_t attr;
    mr_t mr;

    mr_attr_init(&attr);
    mr_attr_set_mapper_threads(&attr, 2);
    mr_attr_set_reducer_threads(&attr, 2);
    mr_attr_set_queue_size(&attr, 4);

    if (mr_create(&mr, &attr, length_mapper, length_reducer, NULL) != 0) {
        fprintf(stderr, "mr_create failed\n");
        return 1;
    }

    if (mr_start(mr, input_path, output_path) != 0) {
        fprintf(stderr, "mr_start failed\n");
        mr_destroy(mr);
        return 1;
    }

    mr_destroy(mr);
    print_output_file(output_path);

    return 0;
}
