#include <unistd.h>
#include <errno.h>

#include "common.h"

int read_full(int fd, void *buf, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t r = read(fd, (char*)buf + off, n - off);
    if (r == 0) return 0; // EOF
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    off += (size_t)r;
  }
  return 1;
}

int write_full(int fd, const void *buf, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t w = write(fd, (const char*)buf + off, n - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    off += (size_t)w;
  }
  return 0;
}