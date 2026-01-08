#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>

#include "debug.h"
#include "server.h"
#include "protocol.h"
#include "board.h"
#include "common.h"

static int pick_first_level(const char *levels_dir, char level_name[256]) {
  DIR *dir = opendir(levels_dir);
  if (!dir) return -1;

  struct dirent *e;
  while ((e = readdir(dir)) != NULL) {
    if (e->d_name[0] == '.') continue;
    const char *dot = strrchr(e->d_name, '.');
    if (dot && strcmp(dot, ".lvl") == 0) {
      strncpy(level_name, e->d_name, 255);
      level_name[255] = '\0';
      closedir(dir);
      return 0;
    }
  }
  closedir(dir);
  return -1;
}

static void* req_reader_thread(void *arg) {
  session_t *sess = (session_t*)arg;
  
  while (1) {
    unsigned char op = 0;

    int read_req = read_full(sess->req_fd, &op, 1);

    if (read_req <= 0) {
      pthread_mutex_lock(&sess->lock);
      sess->disconnected = 1;                  // esta certo???
      pthread_mutex_unlock(&sess->lock);
      return NULL;
    }

    if (op == OP_CODE_DISCONNECT) {
      pthread_mutex_lock(&sess->lock);
      sess->disconnected = 1;                    // esta certo???
      pthread_mutex_unlock(&sess->lock);
      return NULL;
    }

    if (op == OP_CODE_PLAY) {
      unsigned char cmd = 0;
      if (read_full(sess->req_fd, &cmd, 1) != 1) {
        pthread_mutex_lock(&sess->lock);
        sess->disconnected = 1;
        pthread_mutex_unlock(&sess->lock);
        return NULL;
      }

      // “G” desativado na 2ª parte (ignora)
      if ((char)cmd == 'G') continue;

      pthread_mutex_lock(&sess->lock);
      sess->last_cmd = (char)cmd;
      sess->has_cmd = 1;
      pthread_mutex_unlock(&sess->lock);
    }
  }
}

static void* pacman_srv_thread(void *arg) {
  board_t *board = (board_t*)arg;
  pacman_t *pac = &board->pacmans[0];

  while (1) {
    sleep_ms(board->tempo * (1 + pac->passo));

    pthread_mutex_lock(&sess.lock);
    int stop = sess.disconnected || sess.victory || sess.game_over;
    char cmd = sess.has_cmd ? sess.last_cmd : 'T'; // se não há comando, espera
    sess.has_cmd = 0;
    pthread_mutex_unlock(&sess.lock);
    if (stop) return NULL;

    if (cmd == 'Q') { // opcional: permite cliente terminar
      pthread_mutex_lock(&sess.lock);
      sess.game_over = 1;
      pthread_mutex_unlock(&sess.lock);
      return NULL;
    }

    command_t c;
    c.command = cmd;
    c.turns = 1;
    c.turns_left = 1;

    pthread_rwlock_rdlock(&board->state_lock);
    int res = move_pacman(board, 0, &c);
    pthread_rwlock_unlock(&board->state_lock);

    if (!pac->alive || res == DEAD_PACMAN) {
      pthread_mutex_lock(&sess.lock);
      sess.game_over = 1;
      pthread_mutex_unlock(&sess.lock);
      return NULL;
    }
    if (res == REACHED_PORTAL) {
      pthread_mutex_lock(&sess.lock);
      sess.victory = 1;
      pthread_mutex_unlock(&sess.lock);
      return NULL;
    }
  }
}

typedef struct { board_t *board; int idx; } ghost_arg_t;

static void* ghost_srv_thread(void *arg) {
  ghost_arg_t *ga = (ghost_arg_t*)arg;
  board_t *board = ga->board;
  int i = ga->idx;
  free(ga);

  ghost_t *g = &board->ghosts[i];

  while (1) {
    sleep_ms(board->tempo * (1 + g->passo));

    pthread_mutex_lock(&sess.lock);
    int stop = sess.disconnected || sess.victory || sess.game_over;
    pthread_mutex_unlock(&sess.lock);
    if (stop) return NULL;

    pthread_rwlock_rdlock(&board->state_lock);
    move_ghost(board, i, &g->moves[g->current_move % g->n_moves]);
    pthread_rwlock_unlock(&board->state_lock);

    if (!board->pacmans[0].alive) {
      pthread_mutex_lock(&sess.lock);
      sess.game_over = 1;
      pthread_mutex_unlock(&sess.lock);
      return NULL;
    }
  }
}

static void* session_manager_thread(void *arg) {
  board_t *board = (board_t*)arg;
  int n = board->width * board->height;

  char *buf = malloc((size_t)n);
  if (!buf) return NULL;

  while (1) {
    sleep_ms(board->tempo);

    pthread_mutex_lock(&sess.lock);
    int stop = sess.disconnected;
    int victory = sess.victory;
    int game_over = sess.game_over;
    pthread_mutex_unlock(&sess.lock);
    if (stop) break;

    // snapshot do tabuleiro
    pthread_rwlock_rdlock(&board->state_lock);
    for (int i = 0; i < n; i++) buf[i] = board->board[i].content;
    int w = board->width, h = board->height;
    int tempo = board->tempo;
    int points = board->pacmans[0].points;
    pthread_rwlock_unlock(&board->state_lock);

    // OP_CODE_BOARD: OP(1) + 6 ints + board_data[w*h]
    unsigned char op = OP_CODE_BOARD;
    if (write_full(sess.notif_fd, &op, 1) < 0) break;
    if (write_full(sess.notif_fd, &w, sizeof(int)) < 0) break;
    if (write_full(sess.notif_fd, &h, sizeof(int)) < 0) break;
    if (write_full(sess.notif_fd, &tempo, sizeof(int)) < 0) break;
    if (write_full(sess.notif_fd, &victory, sizeof(int)) < 0) break;
    if (write_full(sess.notif_fd, &game_over, sizeof(int)) < 0) break;
    if (write_full(sess.notif_fd, &points, sizeof(int)) < 0) break;
    if (write_full(sess.notif_fd, buf, (size_t)n) < 0) break;
  }

  free(buf);
  return NULL;
}


int main(int argc,char *argv[]) {
    debug("Servidor iniciado...\n");
    // Implementação do servidor aqui

    if (argc != 4) {
        printf("Usage: %s <level_dir> <max_games> <FIFO_name>\n", argv[0]);
        return -1;
    }

    const char *level_dir = argv[1];
    const char *register_pipe = argv[3];

    if (mkfifo(register_pipe, 0666) < 0){
        if (errno != EEXIST) { 
            perror("mkfifo");
            exit(1);
        }
    }

    int max_games = atoi(argv[2]);
    int current_games = 0;

    open_debug_file("debug.log");

    bool end_game = false;
    board_t game_board;
    

    return 0;
}