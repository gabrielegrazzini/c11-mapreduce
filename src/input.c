#include "input.h"
#include "readn_writen.h"
#include "macros.h"
#include "mr_internal.h"

#include <dirent.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>

/* 
Confronta due nomi di file per l'ordinamento lessicografico. 
*/
static int cmp(const void *a, const void *b) {
    const char *s1 = *(const char **)a;
    const char *s2 = *(const char **)b;

    return strcmp(s1, s2);
}

/* 
Conta le entry della directory, escludendo "." e "..". 
*/
static int count_files(const char *path)
{
    DIR *dir = NULL;
    int count = 0;
    struct dirent *entry;

    CHECK_NULL(dir = opendir(path), "count_files: opendir fallita");

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        count++;
    }

    closedir(dir);
    return count;

fail:
    if (dir != NULL) {
        closedir(dir);
    }
    return -1;
}

/* Legge un file riga per riga e invia ogni riga al mapper in formato serializzato. */
static int read_file(int fd, FILE *fp, const char *file_name, size_t* lines_sent){
    char *buffer = NULL;
    size_t len = 0;
    ssize_t n_read;
    int file_name_len = (int)strlen(file_name);
    mr_line_header_t header;
    unsigned long line_number = 0;

    while((n_read = getline(&buffer, &len, fp)) != -1) {
        line_number++;

        /* Rimuove '\n': non fa parte della riga logica passata al mapper. */
        if(n_read > 0 && buffer[n_read - 1] == '\n'){
            n_read--;
        }

        header.file_name_len = file_name_len;
        header.line_number = line_number;
        header.line_len = (int)n_read;

        /* Record riga: header | nome file | riga */
        CHECK_READ_WRITE_FULL(writen(fd, &header, sizeof(header)), sizeof(header), "read_file: scrittura header riga fallita");
        CHECK_READ_WRITE_FULL(writen(fd, file_name, (size_t)header.file_name_len), (size_t)header.file_name_len, "read_file: scrittura file_name fallita");
        CHECK_READ_WRITE_FULL(writen(fd, buffer, (size_t)header.line_len), (size_t)header.line_len, "read_file: scrittura line fallita");

        (*lines_sent)++;
    }

    free(buffer);
    return 0;

fail:
    free(buffer);
    return -1;
}

/* Invia al mapper tutte le righe dell'input, che può essere file regolare o directory. */
int input_send_lines(int fd, const char *input_path) {
    struct stat st;
    DIR *dir = NULL;
    struct dirent *entry;
    char **files = NULL;
    FILE *fp = NULL;
    char *full_path = NULL;
    int count = 0;
    size_t lines_sent = 0;

    /* Identifica il tipo di input. */
    CHECK_MINUS_ONE(stat(input_path, &st), "input_send_lines: stat fallita");

    /* Caso directory */
    if (S_ISDIR(st.st_mode)) {
        CHECK_NULL(dir = opendir(input_path), "input_send_lines: opendir fallita");

        /* Numero di entry da memorizzare. */
        CHECK_MINUS_ONE(count = count_files(input_path), "input_send_lines: count_files fallita");

        if(count > 0){
            CHECK_NULL(files = malloc((size_t)count * sizeof(char*)), "input_send_lines: malloc array file fallita");
        }

        size_t i = 0;

        /* Copia dei nomi nell'array. */
        while((entry = readdir(dir)) != NULL){
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            CHECK_NULL(files[i] = strdup(entry->d_name), "input_send_lines: strdup fallita");
            i++;
        }

        closedir(dir);
        dir = NULL;

        /* Ordine deterministico richiesto dalla specifica. */
        qsort(files, (size_t)count, sizeof(char*), cmp);

        for(int i = 0; i < count; i++){
            /* 
            Costruzione full_path: directory/nome_file. 
            */
            size_t path_len = strlen(input_path) + 1 + strlen(files[i]) + 1;

            CHECK_NULL(full_path = malloc(path_len), "input_send_lines: malloc full_path fallita");

            snprintf(full_path, path_len, "%s/%s", input_path, files[i]);

            /* Apertura e invio righe del file corrente. */
            CHECK_NULL(fp = fopen(full_path, "r"), "input_send_lines: fopen fallita");
            CHECK_MINUS_ONE(log_write("FILE_OPEN", "aperto file input %s", full_path), "log_write");

            CHECK_MINUS_ONE(read_file(fd, fp, files[i], &lines_sent), "input_send_lines: read_file fallita");

            /* pulizia risorse */
            fclose(fp);
            fp = NULL;
            CHECK_MINUS_ONE(log_write("FILE_CLOSE", "chiuso file input %s", full_path), "log_write");


            free(full_path);
            full_path = NULL;

            free(files[i]);
            files[i] = NULL;
        }
        CHECK_MINUS_ONE(log_write("LINES_SENT", "righe inviate al mapper=%zu", lines_sent), "log_write");

        free(files);

        return 0;
    }

    /* Caso file singolo */
    if (S_ISREG(st.st_mode)) {
        const char *file_name = NULL;

        CHECK_NULL(fp = fopen(input_path, "r"), "input_send_lines: fopen fallita");
        CHECK_MINUS_ONE(log_write("FILE_OPEN", "aperto file input %s", input_path), "log_write");

        /* Uso solo il nome del file */
        file_name = strrchr(input_path, '/');
        if (file_name != NULL) {
            file_name++;
        } else {
            file_name = input_path;
        }

        /* Invio le righe del file singolo */
        CHECK_MINUS_ONE(read_file(fd, fp, file_name, &lines_sent), "input_send_lines: read_file fallita");

        fclose(fp);
        CHECK_MINUS_ONE(log_write("FILE_CLOSE", "chiuso file input %s", input_path), "log_write");

        CHECK_MINUS_ONE(log_write("LINES_SENT", "righe inviate al mapper=%zu", lines_sent), "log_write");

        return 0;
    }

    return -1;

fail:
    if (fp != NULL) {
        fclose(fp);
    }

    if (dir != NULL) {
        closedir(dir);
    }

    free(full_path);

    if (files != NULL) {
        for(int i = 0; i < count; i++){
            free(files[i]);
        }
        free(files);
    }

    return -1;
}