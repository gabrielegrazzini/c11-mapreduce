#include "output.h"
#include "readn_writen.h"
#include "macros.h"
#include "mr_internal.h"

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

#define MR_MAX_TOKEN_LEN 4096
#define MR_MAX_RESULT_LEN (64 * 1024 * 1024)

/* 
Legge dal fd i risultati serializzati prodotti dal reducer e li scrive nel file di output
*/
int output_write_lines(int fd, const char *output_path)
{
    int out_fd = -1;
    mr_result_header_t header;
    char *token = NULL;
    void *result = NULL;


    CHECK_MINUS_ONE(out_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644), "output_write_lines: open output fallita");
    CHECK_MINUS_ONE(log_write("FILE_OPEN", "aperto file output %s", output_path), "log_write");

    while (1) {

        /* Ogni record inizia con la lunghezza del token; EOF qui indica che il reducer ha finito. */
        CHECK_READ_FULL_OR_EOF(readn(fd, &header, sizeof(header)), sizeof(header), "output_write_lines: lettura header risultato fallita");

        /* Il token deve essere non vuoto e non deve superare il limite massimo scelto. */
        if (header.token_len <= 0 || header.token_len > MR_MAX_TOKEN_LEN) {
            log_write("ERRORE", "output_write_lines: token_len non valido");
            goto fail;
        }

        /* Il token viene trattato come byte di lunghezza nota: il terminatore nullo non è scritto nel file. */
        token = malloc((size_t)header.token_len);
        CHECK_NULL(token, "output_write_lines: malloc token fallita");
        CHECK_READ_WRITE_FULL(readn(fd, token, (size_t)header.token_len), (size_t)header.token_len, "output_write_lines: lettura token fallita");

        /* Il risultato è una sequenza opaca di byte: può contenere anche byte nulli. */
        if (header.result_len < 0 || header.result_len > MR_MAX_RESULT_LEN) {
            log_write("ERRORE", "output_write_lines: result_len non valido");
            goto fail;
        }

        if (header.result_len > 0) {
            result = malloc((size_t)header.result_len);
            CHECK_NULL(result, "output_write_lines: malloc risultato fallita");
            CHECK_READ_WRITE_FULL(readn(fd, result, (size_t)header.result_len), (size_t)header.result_len, "output_write_lines: lettura risultato fallita");
        }

        /*
        * Riscrive il record nel file di output:
        * lunghezza token | token | lunghezza risultato | risultato
        */
        CHECK_READ_WRITE_FULL(writen(out_fd, &header.token_len, sizeof(header.token_len)), sizeof(header.token_len), "output_write_lines: scrittura token_len fallita");
        CHECK_READ_WRITE_FULL(writen(out_fd, token, (size_t)header.token_len), (size_t)header.token_len, "output_write_lines: scrittura token fallita");
        CHECK_READ_WRITE_FULL(writen(out_fd, &header.result_len, sizeof(header.result_len)), sizeof(header.result_len), "output_write_lines: scrittura result_len fallita");
        if (header.result_len > 0) {
            CHECK_READ_WRITE_FULL(writen(out_fd, result, (size_t)header.result_len), (size_t)header.result_len, "output_write_lines: scrittura risultato fallita");
        }

        free(token);
        free(result);
        token = NULL;
        result = NULL;
    }

eof:
    if (out_fd != -1) {
        CHECK_MINUS_ONE(log_write("FILE_CLOSE", "chiuso file output %s", output_path), "log_write");
        close(out_fd);
    }
    return 0;

fail:
    free(token);
    free(result);
    if (out_fd != -1) {
        CHECK_MINUS_ONE(log_write("FILE_CLOSE", "chiuso file output %s", output_path), "log_write");
        close(out_fd);
    }
    return -1;
}