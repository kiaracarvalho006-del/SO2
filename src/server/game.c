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

typedef struct {
    session_t *session;
    const char* level_dir;
} session_thread_arg_t;

client_queue_t queue; // variavel global da fila de pedidos

static void queue_init(client_queue_t* q) {
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    pthread_mutex_init(&q->mutex, NULL);
    sem_init(&q->sem_empty, 0, MAX_PENDING_CLIENTS);     // todos os slots estão vazios
    sem_init(&q->sem_full, 0, 0);                        // nenhum pedido disponível
}

static void queue_add(client_queue_t* q, client_con_req_t* req) {
    sem_wait(&q->sem_empty);
    pthread_mutex_lock(&q->mutex);

    q->requests[q->tail] = *req;
    q->tail = (q->tail + 1) % MAX_PENDING_CLIENTS;
    q->count++;

    pthread_mutex_unlock(&q->mutex);
    sem_post(&q->sem_full);
}

static client_con_req_t queue_remove(client_queue_t* q) {
    sem_wait(&q->sem_full);
    pthread_mutex_lock(&q->mutex);

    client_con_req_t req = q->requests[q->head];
    q->head = (q->head + 1) % MAX_PENDING_CLIENTS;
    q->count--;

    pthread_mutex_unlock(&q->mutex);
    sem_post(&q->sem_empty);

    return req;
}

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

            pthread_rwlock_wrlock(&board->state_lock);
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

static void* manager_thread(void *arg) {
    int *register_fd = (int*) arg;                    
    
    while (1) {
        client_con_req_t con_req;
        memset(&con_req, 0, sizeof(con_req));

        // le o fd_registo para novas sessões
        unsigned char op = 0; 
                
        // Ler OP code
        if (read_full(*register_fd, &op, 1) != 1) {
            debug("Failed to read op code in manager_thread\n");
            break;
        }
        
        if (op != OP_CODE_CONNECT) {
            debug("Invalid op code in manager_thread: %d\n", op);
            continue;
        }
        
        // Ler caminhos dos FIFOs
        if (read_full(*register_fd, con_req.req_pipe_path, MAX_PIPE_PATH_LENGTH) != MAX_PIPE_PATH_LENGTH ||
            read_full(*register_fd, con_req.notif_pipe_path, MAX_PIPE_PATH_LENGTH) != MAX_PIPE_PATH_LENGTH) {
            debug("Failed to read pipe paths in manager_thread\n");
            break;
        }

        con_req.req_pipe_path[MAX_PIPE_PATH_LENGTH - 1] = '\0';
        con_req.notif_pipe_path[MAX_PIPE_PATH_LENGTH - 1] = '\0';
        debug("[HOST] CONNECT req=%s notif=%s\n", con_req.req_pipe_path, con_req.notif_pipe_path);

        queue_add(&queue, &con_req);
    }
    
    return NULL;
}

static void run_session_game(session_t *sess) {
    int accumulated_points = 0;
    bool end_game = false;
    board_t *game_board = &sess->board;

    DIR* entry_dir = opendir(sess->board.dirname);
    if (!entry_dir) {
        debug("Failed to open levels directory: %s\n", sess->board.dirname);
        return;
    }

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
                if (!ghost_tids) {
                    debug("Failed to allocate ghost_tids\n");
                    end_game = true;
                    break;
                }

                pthread_mutex_lock(&sess->lock);
                sess->shutdown = 0;
                pthread_mutex_unlock(&sess->lock);

                debug("Creating threads\n");

                pthread_create(&pacman_tid, NULL, pacman_thread, (void*) sess);

                for (int i = 0; i < game_board->n_ghosts; i++) {
                    ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
                    arg->session = sess;
                    arg->ghost_index = i;
                    pthread_create(&ghost_tids[i], NULL, ghost_thread, (void*) arg);
                }

                pthread_create(&send_update_tid, NULL, send_board_update_thread, (void*)sess);

                debug("Threads created\n");

                int *retval = NULL;
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
}

static void* session_thread(void *arg) {
    session_thread_arg_t *sess_arg = (session_thread_arg_t*) arg;
    session_t *sess = sess_arg->session;

    pthread_mutex_init(&sess->lock, NULL);
    strncpy(sess->board.dirname, sess_arg->level_dir, MAX_FILENAME);
    sess->board.dirname[MAX_FILENAME - 1] = '\0';

    while (1) {
        debug("Session thread waiting for new connection...\n");
        client_con_req_t con_req = queue_remove(&queue);
        debug("Session thread got new connection: req=%s notif=%s\n", con_req.req_pipe_path, con_req.notif_pipe_path);

        int req_fd = open(con_req.req_pipe_path, O_RDONLY);
        int notif_fd = open(con_req.notif_pipe_path, O_WRONLY);

        unsigned char op = OP_CODE_CONNECT;
        unsigned char result = 0; // sucesso

        if (req_fd < 0 || notif_fd < 0) {
            debug("Failed to open pipes for session\n");
            result = 1; // falha
            if (notif_fd >= 0) {
                write_full(notif_fd, &op, 1);
                write_full(notif_fd, &result, 1);
                close(notif_fd);
            }
            if (req_fd >= 0) close(req_fd);
            continue;
        }

        // enviar resposta de conexão
        if (write_full(notif_fd, &op, 1) < 0 ||
            write_full(notif_fd, &result, 1) < 0) {
            debug("Failed to write connection response for session\n");
            close(req_fd);
            close(notif_fd);
            continue;
        }

        debug("Pipes opened successfully for session\n");

        pthread_mutex_lock(&sess->lock);
        sess->req_fd = req_fd;
        sess->notif_fd = notif_fd;
        sess->disconnected = 0;
        sess->victory = 0;
        sess->game_over = 0;
        sess->shutdown = 0;
        pthread_mutex_unlock(&sess->lock);

        // corre o jogo
        debug("Starting session game...\n");
        run_session_game(sess);

        // cleanup do cliente mas a session continua ativa
        close(req_fd);
        close(notif_fd);

        pthread_mutex_lock(&sess->lock);
        sess->req_fd = -1;
        sess->notif_fd = -1;
        pthread_mutex_unlock(&sess->lock);

        debug("Session ended, waiting for next connection...\n");

    }

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

    const char *level_dir = argv[1];
    int max_games = atoi(argv[2]);  // TODO: implementar limite de jogos
    const char *register_pipe = argv[3];
    

    if (mkfifo(register_pipe, 0666) < 0){
        if (errno != EEXIST) { 
            perror("mkfifo\n");
            exit(1);
        }
    }
    debug("FIFO de registo criado: %s\n", register_pipe);

    int register_fd = open(register_pipe, O_RDONLY);
    int reg_wr_dummy = open(register_pipe, O_WRONLY | O_NONBLOCK); // deixar aberto para sempre
    if (register_fd < 0) {
        perror("open register_pipe\n");
        close_debug_file();
        exit(1);
    }

    queue_init(&queue);

    //host
    pthread_t manager_tid;
    pthread_create(&manager_tid, NULL, manager_thread, (void*)&register_fd);

    //sessions
    pthread_t *session_tid = malloc(max_games * sizeof(pthread_t));
    session_thread_arg_t *session_args = malloc(max_games * sizeof(session_thread_arg_t));
    session_t *sessions = calloc((size_t)max_games, sizeof(session_t));

    for (int i = 0; i < max_games; i++) {
        session_args[i].session = &sessions[i];
        session_args[i].level_dir = level_dir;
        pthread_create(&session_tid[i], NULL, session_thread, (void*)&session_args[i]);
    }

    pthread_join(manager_tid, NULL);

    for (int i = 0; i < max_games; i++) {
        pthread_join(session_tid[i], NULL);
    }

    close(register_fd);
    if (reg_wr_dummy >= 0) close(reg_wr_dummy);

    free(session_tid);
    free(session_args);
    free(sessions);

    close_debug_file();

    return 0;

}