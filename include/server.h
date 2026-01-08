#ifndef SERVER_H
#define SERVER_H

#define MAX_PATH_LENGTH 256

typedef struct {
  int req_fd;     // servidor lê OP_PLAY/OP_DISCONNECT
  int notif_fd;   // servidor escreve OP_BOARD

  board_t board;

  pthread_mutex_t lock;
  int disconnected;
  int victory;
  int game_over;

  char last_cmd;
  int has_cmd;

  int shutdown;     // global stop flag for session threads
} session_t;

#endif 