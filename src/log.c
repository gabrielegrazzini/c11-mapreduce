#include <stdio.h>
#include <semaphore.h>
#include <time.h>
#include <threads.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>


static FILE *log_file = NULL;
static sem_t *log_sem = NULL;
static struct timeval log_start;

/* Inizializza il file di log e il semaforo usato per sincronizzare le scritture tra processi/thread. */
int log_init(const char *path) {

    /* Tempo iniziale usato per stampare timestamp relativi. */
    gettimeofday(&log_start, NULL);

    /* File di log condiviso dai processi dopo fork. */
    log_file = fopen(path, "w");
    if (log_file == NULL) return -1;

    /* Creazione semaforo */
    log_sem = sem_open("/mr_log", O_CREAT, 0666, 1);
    if (log_sem == SEM_FAILED) return -1;

    return 0;
}

/* Scrive una riga di log con tempo relativo, pid, thread id, evento e messaggio formattato. */
int log_write(const char* event, const char* message, ...){
    if(sem_wait(log_sem) == -1) return -1;

    /* Calcolo tempo trascorso dall'inizio dell'elaborazione. */
    struct timeval now;
    gettimeofday(&now, NULL);
    double elapsed = (now.tv_sec - log_start.tv_sec) + (now.tv_usec - log_start.tv_usec) / 1e6;

    pid_t pid = getpid();
    thrd_t tid = thrd_current();

    va_list args;
    va_start(args, message);

    /* Formato:
     * [tempo] [pid] [thread] [evento] messaggio
     */
    fprintf(log_file, "[%.6f] [%d] [%lu] [%s] ",elapsed,pid,(unsigned long)tid,event);

    vfprintf(log_file, message, args);
    fprintf(log_file, "\n");
    va_end(args);
    fflush(log_file);

    if(sem_post(log_sem) == -1) return -1;

    return 0;
}

/* Chiude il file di log e rilascia il semaforo usato per sincronizzare le scritture. */
int log_close(void){
    fclose(log_file);
    if(sem_close(log_sem) == -1) return -1;
    if(sem_unlink("/mr_log") == -1) return -1;

    return 0;
}

