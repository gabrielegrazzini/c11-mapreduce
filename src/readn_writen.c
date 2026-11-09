#include "readn_writen.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include "macros.h"

/* 
Legge esattamente n byte da fd dentro buf, ripetendo read() finché
il buffer è pieno, arriva EOF o si verifica un errore. 
*/
ssize_t readn(int fd, void * buf, size_t n){
    ssize_t letti;
    size_t letti_totali = 0;

    while(letti_totali != n){
        CHECK_MINUS_ONE(letti = read(fd, (char*)buf + letti_totali, n - letti_totali), "readn: read");
        if(letti == 0) return letti_totali;
        letti_totali += letti;
    }
    return letti_totali;
fail:
    return -1;
}

/* 
Scrive esattamente n byte da buf su fd, ripetendo write() finché 
tutti i byte sono stati scritti o si verifica un errore. 
*/
ssize_t writen(int fd, const void* buf, size_t n){
    ssize_t scritti;
    size_t scritti_totali = 0;

    while(scritti_totali != n){
        CHECK_MINUS_ONE(scritti = write(fd, (char*)buf + scritti_totali, n - scritti_totali), "writen: read");
        if(scritti == 0) return scritti_totali;
        scritti_totali += scritti;
    }
    return scritti_totali;

fail:
    return -1;
}