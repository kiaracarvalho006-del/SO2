#include "board.h"
#include "display.h"
#include "debug.h"
#include "common.h"
#include "protocol.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <errno.h>

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

typedef struct {
    session_t *session;
    int ghost_index;
} ghost_thread_arg_t;

void screen_refresh(board_t * game_board, int mode) {
    debug("REFRESH\n");
    draw_board(game_board, mode);
    refresh_screen();     
}

/**void* ncurses_thread(void *arg) {
    board_t *board = (board_t*) arg;
    sleep_ms(board->tempo / 2);
    while (true) {
        sleep_ms(board->tempo);
        pthread_rwlock_wrlock(&board->state_lock);
        if (sess->shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        screen_refresh(board, DRAW_MENU);
        pthread_rwlock_unlock(&board->state_lock);
    }
}*/

void* pacman_thread(void *arg) {
    session_t *sess = (session_t*) arg;
    board_t *board = &sess->board;

    pacman_t* pacman = &board->pacmans[0];

    int *retval = malloc(sizeof(int));

    pthread_mutex_lock(&sess->lock);
    if (sess->shutdown) {
        pthread_mutex_unlock(&sess->lock);
        pthread_exit(NULL);
    }
    pthread_mutex_unlock(&sess->lock);

    while (true) {
        sleep_ms(board->tempo * (1 + pacman->passo));

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
            *retval = QUIT_GAME;          
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

            // “G” desativado (ignora)
            if ((char)cmd == 'G') continue;

            pthread_mutex_lock(&sess->lock);
            //TODO: tratar comandos concorrentes
            //sess->last_cmd = (char)cmd;
            //sess->has_cmd = 1;
            pthread_mutex_unlock(&sess->lock);

            command_t play;
            play.command = (char)cmd;
            
            debug("KEY %c\n", play.command);

            // QUIT
            if (play.command == 'Q') {
                *retval = QUIT_GAME;
                return (void*) retval;
            }

            pthread_rwlock_rdlock(&board->state_lock);
            int result = move_pacman(board, 0, &play);
            pthread_rwlock_unlock(&board->state_lock);

            if (result == REACHED_PORTAL) {
                *retval = NEXT_LEVEL;
                break;
            }

            if(result == DEAD_PACMAN) {
                *retval = QUIT_GAME;
                break;
            }
        }            
    }
    return (void*) retval;
}

void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    board_t *board = &ghost_arg->session->board;
    session_t *sess = ghost_arg->session;
    int ghost_ind = ghost_arg->ghost_index;

    free(ghost_arg);

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (true) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_mutex_lock(&sess->lock);
        int stop = sess->shutdown;
        pthread_mutex_unlock(&sess->lock);

        if (stop) {
            pthread_exit(NULL);
        }
        
        pthread_rwlock_wrlock(&board->state_lock);
        int result = move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move%ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);

        if (result == DEAD_PACMAN) {
            pthread_mutex_lock(&sess->lock);
            sess->game_over = 1;
            pthread_mutex_unlock(&sess->lock);
            debug("Game over set to 1 by ghost\n");
        }
    }
}

int send_board_update(session_t *sess) {
    board_t *board = &sess->board;
    int n = board->width * board->height;
    
    char *buf = malloc((size_t)n);
    if (!buf) return -1;
    
    // snapshot do tabuleiro
    pthread_rwlock_rdlock(&board->state_lock);
    for (int i = 0; i < n; i++) {
        // Dados do tabuleiro (converter para formato do cliente)
        char ch = board->board[i].content;
        
        // Converter caracteres internos para formato de display
        switch (ch) {
            case 'W': // Wall
                buf[i] = '#';
                break;
            case 'P': // Pacman
                buf[i] = 'C';
                break;
            case 'M': // Monster/Ghost
                buf[i] = 'M';
                break;
            case ' ': // Empty space
                if (board->board[i].has_portal) {
                    buf[i] = '@';
                } else if (board->board[i].has_dot) {
                    buf[i] = '.';
                } else {
                    buf[i] = ' ';
                }
                break;
            default:
                buf[i] = ch;
                break;
        }
    }
   
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
    if (write_full(sess->notif_fd, &op, 1) < 0) { debug("Failed to write op code"); free(buf); return -1; }
    if (write_full(sess->notif_fd, &w, sizeof(int)) < 0) { free(buf); return -1; }
    if (write_full(sess->notif_fd, &h, sizeof(int)) < 0) { free(buf); return -1; }
    if (write_full(sess->notif_fd, &tempo, sizeof(int)) < 0) { free(buf); return -1; }
    if (write_full(sess->notif_fd, &victory, sizeof(int)) < 0) { free(buf); return -1; }
    if (write_full(sess->notif_fd, &game_over, sizeof(int)) < 0) { free(buf); return -1; }
    if (write_full(sess->notif_fd, &points, sizeof(int)) < 0) { free(buf); return -1; }
    if (write_full(sess->notif_fd, buf, (size_t)n) < 0) { free(buf); return -1; }

    free(buf);

    return 0;
}

static void* send_board_update_thread(void *arg) {
    session_t *sess = (session_t*)arg;
    
    // update inicial
    debug("Sending initial board update\n");
    if (send_board_update(sess) < 0) {
        debug("Failed to send initial board update\n");
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

static void* session_thread(void *arg) {
    session_t *sess = (session_t*)arg;

    int accumulated_points = 0;
    bool end_game = false;
    board_t *game_board = &sess->board;

    DIR* entry_dir = opendir(sess->board.dirname);
    struct dirent* entry;

    while ((entry = readdir(entry_dir)) != NULL && !end_game) {
        debug("Checking file: %s\n", entry->d_name);
        if (entry->d_name[0] == '.') continue;

        char *dot = strrchr(entry->d_name, '.');
        if (!dot) continue;

        if (strcmp(dot, ".lvl") == 0) {
            pthread_mutex_lock(&sess->lock);
            sess->victory = 0;
            sess->game_over = 0;
            pthread_mutex_unlock(&sess->lock);
            load_level(sess, entry->d_name, sess->board.dirname, accumulated_points);

            while(true) {
                pthread_t pacman_tid, send_update_tid;
                pthread_t *ghost_tids = malloc(game_board->n_ghosts * sizeof(pthread_t));

                sess->shutdown = 0;

                debug("Creating threads\n");

                pthread_create(&pacman_tid, NULL, pacman_thread, (void*) sess);
                for (int i = 0; i < game_board->n_ghosts; i++) {
                    ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
                    arg->session = sess;
                    arg->ghost_index = i;
                    pthread_create(&ghost_tids[i], NULL, ghost_thread, (void*) arg);
                }
                //pthread_create(&ncurses_tid, NULL, ncurses_thread, (void*) &game_board);

                pthread_create(&send_update_tid, NULL, send_board_update_thread, (void*)sess);
                debug("Threads created\n");

                int *retval;
                pthread_join(pacman_tid, (void**)&retval);

                pthread_mutex_lock(&sess->lock);
                sess->shutdown = 1;
                pthread_mutex_unlock(&sess->lock);


                //pthread_join(ncurses_tid, NULL);
                for (int i = 0; i < game_board->n_ghosts; i++) {
                    pthread_join(ghost_tids[i], NULL);
                }
                pthread_join(send_update_tid, NULL);

                free(ghost_tids);

                int result = *retval;
                free(retval);

                if(result == NEXT_LEVEL) {
                    pthread_mutex_lock(&sess->lock);
                    sess->victory = 1;
                    pthread_mutex_unlock(&sess->lock);
                    accumulated_points = sess->board.pacmans[0].points;
                    break;
                }

                if(result == QUIT_GAME) {
                    pthread_mutex_lock(&sess->lock);
                    sess->game_over = 1;
                    debug("Game over set to 1\n");
                    pthread_mutex_unlock(&sess->lock);
                    end_game = true;
                    send_board_update(sess);
                    break;
                }

                accumulated_points = sess->board.pacmans[0].points;      
            }
            unload_level(game_board);
        }
    }   
    // Enviar board final com victory flag antes de sair
    send_board_update(sess);
    closedir(entry_dir);
    return NULL;
}

int main(int argc,char *argv[]) {
    if (argc != 4) {
        printf("Usage: %s <level_dir> <max_games> <FIFO_name>\n", argv[0]);
        return -1;
    }

    // Abrir arquivo de debug PRIMEIRO
    open_debug_file("debug.log");
    debug("Servidor iniciado...\n");

    // int max_games = atoi(argv[2]);  // TODO: implementar limite de jogos
    const char *level_dir = argv[1];
    const char *register_pipe = argv[3];
    

    if (mkfifo(register_pipe, 0666) < 0){
        if (errno != EEXIST) { 
            perror("mkfifo\n");
            exit(1);
        }
    }
    debug("FIFO de registo criado: %s\n", register_pipe);

    //int current_games = 0;
    int op = 0;
    int result = -1; // Placeholder for connect result
    char req_pipe[MAX_PIPE_PATH_LENGTH];
    char notif_pipe[MAX_PIPE_PATH_LENGTH];

    int register_fd = open(register_pipe, O_RDONLY);
    if (register_fd < 0) {
        perror("open register_pipe\n");
        close_debug_file();
        exit(1);
    }

    if (read_full(register_fd, &op, 1) > 0) {
        if (op != OP_CODE_CONNECT) {
            close(register_fd);
            debug("Operação inválida no pipe de registo\n");
            return -1;
        }
        result = 0; // Placeholder for connect result
        read_full(register_fd, req_pipe, MAX_PIPE_PATH_LENGTH);
        read_full(register_fd, notif_pipe, MAX_PIPE_PATH_LENGTH);
    }

    debug("Novo cliente: req_pipe=%s, notif_pipe=%s\n", req_pipe, notif_pipe);

    int req_pipe_fd = open(req_pipe, O_RDONLY);
    int notif_pipe_fd = open(notif_pipe, O_WRONLY);

    unsigned char result_byte = (unsigned char)result;
    write_full(notif_pipe_fd, &op, 1);   //rever
    write_full(notif_pipe_fd, &result_byte, 1);

    session_t session;
    memset(&session, 0, sizeof(session));
    
    session.req_fd = req_pipe_fd;
    session.notif_fd = notif_pipe_fd;
    pthread_t session_tid;

    strncpy(session.board.dirname, level_dir, MAX_FILENAME);
    session.board.dirname[MAX_FILENAME - 1] = '\0';

    pthread_mutex_init(&session.lock, NULL);

    pthread_create(&session_tid, NULL, session_thread, (void*)&session);

    pthread_join(session_tid, NULL);  

    close(register_fd);
    close(req_pipe_fd);
    close(notif_pipe_fd);

    return 0;
}






/* TODO: implementar manager_thread para múltiplas sessões
static void* manager_thread(void *arg) {
    int *register_fd = (int*)arg;
    
    while (1) {
        // le o fd_registo para novas sessões
        unsigned char op = 0;
        char req_pipe[MAX_PIPE_PATH_LENGTH];
        char notif_pipe[MAX_PIPE_PATH_LENGTH];
        
        // Ler OP code
        if (read_full(*register_fd, &op, 1) != 1) {
            break;
        }
        
        if (op != OP_CODE_CONNECT) {
            continue;
        }
        
        // Ler caminhos dos FIFOs
        if (read_full(*register_fd, req_pipe, MAX_PIPE_PATH_LENGTH) != 1 ||
            read_full(*register_fd, notif_pipe, MAX_PIPE_PATH_LENGTH) != 1) {
            break;
        }
        
        // cria nova sessão se possível
        // TODO: verificar se não excedeu max_games
        session_t *new_sess = malloc(sizeof(session_t));
        if (!new_sess) {
            continue;
        }
        
        memset(new_sess, 0, sizeof(session_t));
        // session_t não tem req_pipe_path nem notif_pipe_path
        
        // abre os FIFOs de notificação e requisição
        new_sess->req_fd = open(req_pipe, O_RDONLY);
        new_sess->notif_fd = open(notif_pipe, O_WRONLY);
        
        // fecha os fifos se não for possível
        if (new_sess->req_fd < 0 || new_sess->notif_fd < 0) {
            if (new_sess->req_fd >= 0) close(new_sess->req_fd);
            if (new_sess->notif_fd >= 0) close(new_sess->notif_fd);
            free(new_sess);
            continue;
        }
        
        pthread_mutex_init(&new_sess->lock, NULL);
        
        // TODO: criar threads para esta sessão (req_reader_thread, send_board_update_thread)
        // TODO: adicionar à lista de sessões ativas
    }
    
    return NULL;
}
*/