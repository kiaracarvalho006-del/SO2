#include "board.h"
#include "display.h"
#include "debug.h"
#include "common.h"
#include "protocol.h"
#include "server.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

typedef struct {
    session_t *session;
    int ghost_index;
} ghost_thread_arg_t;

int thread_shutdown = 0;

int create_backup() {
    // clear the terminal for process transition
    terminal_cleanup();

    pid_t child = fork();

    if(child != 0) {
        if (child < 0) {
            return -1;
        }

        return child;
    } else {
        debug("[%d] Created\n", getpid());

        return 0;
    }
}

void screen_refresh(board_t * game_board, int mode) {
    debug("REFRESH\n");
    draw_board(game_board, mode);
    refresh_screen();     
}

void* ncurses_thread(void *arg) {
    board_t *board = (board_t*) arg;
    sleep_ms(board->tempo / 2);
    while (true) {
        sleep_ms(board->tempo);
        pthread_rwlock_wrlock(&board->state_lock);
        if (thread_shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        screen_refresh(board, DRAW_MENU);
        pthread_rwlock_unlock(&board->state_lock);
    }
}

void* pacman_thread(void *arg) {
    session_t *sess = (session_t*) arg;
    board_t *board = &sess->board;

    pacman_t* pacman = &board->pacmans[0];

    int *retval = malloc(sizeof(int));

    while (true) {
        sleep_ms(board->tempo * (1 + pacman->passo));

        command_t* play;
        command_t c;
        if (pacman->n_moves == 0) {
            c.command = get_input();

            if(c.command == '\0') {
                continue;
            }

            c.turns = 1;
            play = &c;
        }
        else {
            play = &pacman->moves[pacman->current_move%pacman->n_moves];
        }

        debug("KEY %c\n", play->command);

        // QUIT
        if (play->command == 'Q') {
            *retval = QUIT_GAME;
            return (void*) retval;
        }

        pthread_rwlock_rdlock(&board->state_lock);

        int result = move_pacman(board, 0, play);
        if (result == REACHED_PORTAL) {
            // Next level
            *retval = NEXT_LEVEL;
            break;
        }

        if(result == DEAD_PACMAN) {
            break;
        }

        pthread_rwlock_unlock(&board->state_lock);
    }
    pthread_rwlock_unlock(&board->state_lock);
    return (void*) retval;
}

void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;

    free(ghost_arg);

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (true) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_rdlock(&board->state_lock);
        if (thread_shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        
        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move%ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        printf("Usage: %s <level_directory>\n", argv[0]);
        return -1;
    }

    // Random seed for any random movements
    srand((unsigned int)time(NULL));

    DIR* level_dir = opendir(argv[1]);
        
    if (level_dir == NULL) {
        fprintf(stderr, "Failed to open directory: %s\n", argv[1]);
        return 0;
    }

    open_debug_file("debug.log");

    terminal_init();
    
    int accumulated_points = 0;
    bool end_game = false;
    board_t game_board;

    pid_t parent_process = getpid(); // Only the parent process can create backups

    struct dirent* entry;
    while ((entry = readdir(level_dir)) != NULL && !end_game) {
        if (entry->d_name[0] == '.') continue;

        char *dot = strrchr(entry->d_name, '.');
        if (!dot) continue;

        if (strcmp(dot, ".lvl") == 0) {
            load_level(&game_board, entry->d_name, argv[1], accumulated_points);
            draw_board(&game_board, DRAW_MENU);
            refresh_screen();

            while(true) {
                pthread_t ncurses_tid, pacman_tid;
                pthread_t *ghost_tids = malloc(game_board.n_ghosts * sizeof(pthread_t));

                thread_shutdown = 0;

                debug("Creating threads\n");

                pthread_create(&pacman_tid, NULL, pacman_thread, (void*) &game_board);
                for (int i = 0; i < game_board.n_ghosts; i++) {
                    ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
                    arg->board = &game_board;
                    arg->ghost_index = i;
                    pthread_create(&ghost_tids[i], NULL, ghost_thread, (void*) arg);
                }
                pthread_create(&ncurses_tid, NULL, ncurses_thread, (void*) &game_board);

                int *retval;
                pthread_join(pacman_tid, (void**)&retval);

                pthread_rwlock_wrlock(&game_board.state_lock);
                thread_shutdown = 1;
                pthread_rwlock_unlock(&game_board.state_lock);

                pthread_join(ncurses_tid, NULL);
                for (int i = 0; i < game_board.n_ghosts; i++) {
                    pthread_join(ghost_tids[i], NULL);
                }

                free(ghost_tids);

                int result = *retval;
                free(retval);

                if(result == NEXT_LEVEL) {
                    screen_refresh(&game_board, DRAW_WIN);
                    sleep_ms(game_board.tempo);
                    break;
                }

                if(result == QUIT_GAME) {
                    screen_refresh(&game_board, DRAW_GAME_OVER); 
                    sleep_ms(game_board.tempo);
                    end_game = true;
                    break;
                }
      
                screen_refresh(&game_board, DRAW_MENU); 

                accumulated_points = game_board.pacmans[0].points;      
            }
            print_board(&game_board);
            unload_level(&game_board);
        }
    }    

    terminal_cleanup();

    close_debug_file();

    if (closedir(level_dir) == -1) {
        fprintf(stderr, "Failed to close directory\n");
        return 0;
    }
    return 0;
}

static void* req_reader_thread(void *arg) {
  session_t *sess = (session_t*)arg;

  while (1) {
    unsigned char op = 0;

    if (read_full(sess->req_fd, &op, 1) != 1) {
        pthread_mutex_lock(&sess->lock);
        sess->disconnected = 1;                  
        pthread_mutex_unlock(&sess->lock);
        return NULL;
    }

    if (op == OP_CODE_DISCONNECT) {
        pthread_mutex_lock(&sess->lock);
        sess->disconnected = 1;                    
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

static void send_board_update(session_t *sess) {
    board_t *board = &sess->board;
    int n = board->width * board->height;
    
    char *buf = malloc((size_t)n);
    if (!buf) return;
    
    // snapshot do tabuleiro
    pthread_rwlock_rdlock(&board->state_lock);
    for (int i = 0; i < n; i++) buf[i] = board->board[i].content;
    int w = board->width, h = board->height;
    int tempo = board->tempo;
    int points = board->pacmans[0].points;
    pthread_rwlock_unlock(&board->state_lock);
    
    pthread_mutex_lock(&sess->lock);
    int victory, game_over;
    victory = sess->victory;
    game_over = sess->game_over;
    pthread_mutex_unlock(&sess->lock);
    
    // OP_CODE_BOARD: OP(1) + 6 ints + board_data[w*h]
    unsigned char op = OP_CODE_BOARD;
    if (write_full(sess->notif_fd, &op, 1) < 0) goto cleanup;
    if (write_full(sess->notif_fd, &w, sizeof(int)) < 0) goto cleanup;
    if (write_full(sess->notif_fd, &h, sizeof(int)) < 0) goto cleanup;
    if (write_full(sess->notif_fd, &tempo, sizeof(int)) < 0) goto cleanup;
    if (write_full(sess->notif_fd, &victory, sizeof(int)) < 0) goto cleanup;
    if (write_full(sess->notif_fd, &game_over, sizeof(int)) < 0) goto cleanup;
    if (write_full(sess->notif_fd, &points, sizeof(int)) < 0) goto cleanup;
    if (write_full(sess->notif_fd, buf, (size_t)n) < 0) goto cleanup;

    cleanup:
        free(buf);
}

static void send_board_update_thread(session_t *sess){
    session_t *sess = (session_t*)arg;
    
    // update inicial
    if (send_board_update(sess) < 0) {
        pthread_mutex_lock(&sess->lock);
        sess->disconnected = 1;
        sess->shutdown = 1;
        pthread_mutex_unlock(&sess->lock);
        return NULL;
    }

    while (1) {
        pthread_mutex_lock(&sess->lock);
        int stop = sess->shutdown;
        pthread_mutex_unlock(&sess->lock);
        if (stop) break;

        sleep_ms(sess->board.tempo);

        if (send_board_update(sess) < 0) {
            pthread_mutex_lock(&sess->lock);
            sess->disconnected = 1;
            sess->shutdown = 1;
            pthread_mutex_unlock(&sess->lock);
            break;
        }
    }

    // update final (opcional, mas útil)
    (void)send_board_update(sess);
    return NULL;
}

static void manager_thread() {
    while (1) {
        sleep_ms(sess->board.tempo);

        pthread_mutex_lock(&sess->lock);
        int stop = sess->disconnected;
        pthread_mutex_unlock(&sess->lock);
        if (stop) break;
        // le o fd_registo para novas sessões
        // cria nova sessão se possível
        // abre os FIFOs de notificação e requisição

        
        
    }
}

static void* session_thread(void *arg) {
  session_t *sess = (session_t*)arg;
  int n = sess->board.width * sess->board.height;

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

    int register_fd = open(register_pipe, O_RDONLY);

    int max_games = atoi(argv[2]);
    int current_games = 0;

    open_debug_file("debug.log");

    bool end_game = false;
    board_t game_board;
    

    return 0;
}
