#include "api.h"
#include "protocol.h"
#include "debug.h"
#include "common.h"

#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>


struct Session {
  int id;
  int req_pipe;
  int notif_pipe;
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
};

static struct Session session = {.id = -1, .req_pipe = -1, .notif_pipe = -1};

static int make_fifo_if_needed(const char *path) {
  if (mkfifo(path, 0666) < 0) {
    if (errno == EEXIST) return 0;
    return -1;
  }
  return 0;
}

int pacman_connect(const char *req_pipe_path, const char *notif_pipe_path, const char *server_pipe_path){
  
  // guardar paths
  strncpy(session.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
  session.req_pipe_path[MAX_PIPE_PATH_LENGTH] = '\0';
  strncpy(session.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);
  session.notif_pipe_path[MAX_PIPE_PATH_LENGTH] = '\0';

  // limpar restos (opcional)
  unlink(session.req_pipe_path);
  unlink(session.notif_pipe_path);

  // criar FIFOs do cliente
  if (make_fifo_if_needed(session.req_pipe_path) < 0) return 1;
  if (make_fifo_if_needed(session.notif_pipe_path) < 0) {
    unlink(session.req_pipe_path);
    return 1;
  }

  // abrir FIFO do servidor e enviar CONNECT: OP(1) | req | notif
  int reg_fd = open(server_pipe_path, O_WRONLY);
  if (reg_fd < 0) goto fail_fifos;

  unsigned char op = OP_CODE_CONNECT;

  char req40[MAX_PIPE_PATH_LENGTH];
  char notif40[MAX_PIPE_PATH_LENGTH];
  memset(req40, 0, sizeof(req40));
  memset(notif40, 0, sizeof(notif40));
  strncpy(req40, session.req_pipe_path, MAX_PIPE_PATH_LENGTH - 1);
  strncpy(notif40, session.notif_pipe_path, MAX_PIPE_PATH_LENGTH - 1);

  if (write_full(reg_fd, &op, 1) < 0 ||
      write_full(reg_fd, req40, sizeof(req40)) < 0 ||
      write_full(reg_fd, notif40, sizeof(notif40)) < 0) {
    close(reg_fd);
    goto fail_fifos;
  }
  close(reg_fd);

  // abrir req primeiro (evita deadlock)
  session.req_pipe = open(session.req_pipe_path, O_WRONLY);
  if (session.req_pipe < 0) goto fail_fifos;

  // abrir notif
  session.notif_pipe = open(session.notif_pipe_path, O_RDONLY);
  if (session.notif_pipe < 0) {
    close(session.req_pipe);
    session.req_pipe = -1;
    goto fail_fifos;
  }

  unsigned char ack_op = 0, result = 1;
  if (read_full(session.notif_pipe, &ack_op, 1) != 1 || 
      read_full(session.notif_pipe, &result, 1) != 1 || 
      ack_op != OP_CODE_CONNECT || result != 0) {
    close(session.req_pipe);
    close(session.notif_pipe);
    session.req_pipe = -1;
    session.notif_pipe = -1;
    goto fail_fifos;
  }

  session.id = 0;
  return 0;

  fail_fifos:
    unlink(session.req_pipe_path);
    unlink(session.notif_pipe_path);
    session.req_pipe_path[0] = '\0';
    session.notif_pipe_path[0] = '\0';
    return 1;
}

int pacman_play(char command) {

  if (session.req_pipe < 0) return -1;

  unsigned char op = OP_CODE_PLAY;
  unsigned char cmd = (unsigned char)command;

  if (write_full(session.req_pipe, &op, 1) < 0) return -1;
  if (write_full(session.req_pipe, &cmd, 1) < 0) return -1;

  return 0; 
}

int pacman_disconnect() {
  if (session.req_pipe >= 0) {
    unsigned char op = OP_CODE_DISCONNECT;
    (void)write_full(session.req_pipe, &op, 1);
  }

  if (session.req_pipe >= 0) close(session.req_pipe);
  if (session.notif_pipe >= 0) close(session.notif_pipe);

  session.req_pipe = -1;
  session.notif_pipe = -1;
  session.id = -1;

  if (session.req_pipe_path[0] != '\0') unlink(session.req_pipe_path);
  if (session.notif_pipe_path[0] != '\0') unlink(session.notif_pipe_path);

  session.req_pipe_path[0] = '\0';
  session.notif_pipe_path[0] = '\0';

  return 0;
}

Board receive_board_update(void) {
  Board board;
  memset(&board, 0, sizeof(Board));
  
  if (session.notif_pipe < 0) {
    debug("Notification pipe not open\n");
    return board;
  }

  unsigned char op = 0;
  if (read_full(session.notif_pipe, &op, 1) != 1) {
    debug("EOF or error reading op; stopping client receiver\n");
    board.data = NULL;
    return board;
  }
  debug("Received op=%d\n", op);
  if (op != OP_CODE_BOARD) { debug("Invalid op code, expected %d\n", OP_CODE_BOARD); return board; }

  if (read_full(session.notif_pipe, &board.width, sizeof(int)) != 1) return board;
  if (read_full(session.notif_pipe, &board.height, sizeof(int)) != 1) return board;
  if (read_full(session.notif_pipe, &board.tempo, sizeof(int)) != 1) return board;
  if (read_full(session.notif_pipe, &board.victory, sizeof(int)) != 1) return board;
  if (read_full(session.notif_pipe, &board.game_over, sizeof(int)) != 1) return board;
  if (read_full(session.notif_pipe, &board.accumulated_points, sizeof(int)) != 1) return board;
  int n = board.width * board.height;
  if (n <= 0) return board;

  board.data = (char*)malloc((size_t)(n + 1));
  if (!board.data) {
    return board;
  }

  if (read_full(session.notif_pipe, board.data, (size_t)n) != 1) {
    free(board.data);
    board.data = NULL;
    return board;
  }
  
  return board;
}