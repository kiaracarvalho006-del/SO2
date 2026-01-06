#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int write_full(int fd, const void *buf, size_t n);

int read_full(int fd, void *buf, size_t n);


#endif // COMMON_H