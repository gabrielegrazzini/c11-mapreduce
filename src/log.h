#ifndef LOG_H
#define LOG_H

int log_init(const char *path);
int log_write(const char* event, const char* message, ...);
int log_close(void);

#endif