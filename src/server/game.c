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
#include <signal.h>
#include <ctype.h>

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

static volatile sig_atomic_t got_sigusr1 = 0;

static void on_sigusr1(int sig) {
    (void)sig; // evitar warning de variável não usada
    got_sigusr1 = 1;
}

typedef struct {
    session_t *session;
    int ghost_index;
} ghost_thread_arg_t;

typedef struct {
    session_t *session;
    const char* level_dir;
} session_thread_arg_t;

typedef struct {
    int *register_fd;
    session_t *sessions;
    int max_games;
} manager_thread_arg_t;

typedef struct {
    int id;
    int points;
} top_player_t;

client_queue_t queue; // variavel global da fila de pedidos

static int cmp_top_players(const void *a, const void *b) {
    const top_player_t *playerA = (top_player_t *)a;
    const top_player_t *playerB = (top_player_t *)b;

    if (playerA->points != playerB->points) return playerB->points - playerA->points; 
    return playerA->id - playerB->id;
}

static void dump_top5(session_t *sessions, int max_games) {
    top_player_t *top_players = malloc((size_t)max_games * sizeof(top_player_t));
    if (!top_players) return;

    int count = 0;
    for (int i = 0; i < max_games; i++) {
        session_t *sess = &sessions[i];

        // verificar se "com sessão ativa"
        pthread_mutex_lock(&sessions[i].lock);
        int req_fd = sess->req_fd;
        int notif_fd = sess->notif_fd;
        int disconnected = sess->disconnected;
        int client_id = sess->client_id;
        pthread_mutex_unlock(&sessions[i].lock);

        if (req_fd < 0 || notif_fd < 0 || disconnected) continue;

        pthread_rwlock_rdlock(&sess->board.state_lock);
        int points = (sess->board.n_pacmans > 0) ? sess->board.pacmans[0].points : 0;
        pthread_rwlock_unlock(&sess->board.state_lock);

        top_players[count].id = client_id;
        top_players[count].points = points;
        count++;
    }

    qsort(top_players, (size_t)count, sizeof(top_player_t), cmp_top_players);

    FILE *f = fopen("top5.txt", "w");
    if (!f) {
        free(top_players);
        return;
    }

    int limit = count < 5 ? count : 5;

    debug("Top 5 Players:\n");
    for (int i = 0; i < limit; i++) {
        debug("%d. Player ID: %d, Points: %d\n", i + 1, top_players[i].id, top_players[i].points);
        fprintf(f, "%d %d\n", top_players[i].id, top_players[i].points);
    }

    fclose(f);
    free(top_players);
}

static int read_full_host(int fd, void *buf, size_t n, session_t *sessions, int max_games) {
    size_t off = 0;

    while (off < n) {
        ssize_t r = read(fd, (char*)buf + off, n - off);

        if (r == 0) return 0; // EOF

        if (r < 0) {
            if (errno == EINTR) {
                // sinal interrompeu: se foi SIGUSR1, cria o ficheiro
                if (got_sigusr1) {
                    debug("SIGUSR1 received, dumping top 5 players...\n");
                    got_sigusr1 = 0;
                    dump_top5(sessions, max_games);
                }
                continue; // volta a tentar ler o que faltava
            }
            return -1; // erro real
        }

        off += (size_t)r;
    }

    return 1;
}

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

static int exctract_client_id(const char* pipe_path) {
    const char* base = strrchr(pipe_path, '/');
    base = base ? base + 1 : pipe_path;

    while (*base && !isdigit((unsigned char)*base)) base++;
    
    if (!*base) return -1;

    char *endptr = NULL;
    long v = strtol(base, &endptr, 10);
    if (endptr == base || v < 0 || v > 1000000000L) return -1;
    return (int)v;
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
            *retval = QUIT_GAME;
            return (void*) retval;
        }

        if (op == OP_CODE_DISCONNECT) {
            pthread_mutex_lock(&sess->lock);
            sess->disconnected = 1;          
            pthread_mutex_unlock(&sess->lock);
            *retval = QUIT_GAME;
            return (void*) retval;
        }

        if (op == OP_CODE_PLAY) {
            unsigned char cmd = 0;
            if (read_full(sess->req_fd, &cmd, 1) != 1) {
                pthread_mutex_lock(&sess->lock);
                sess->disconnected = 1;
                pthread_mutex_unlock(&sess->lock);
                *retval = QUIT_GAME;
                return (void*) retval;
            }

            // “G” desativado (ignora)
            if ((char)cmd == 'G') continue;

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
    
    pthread_rwlock_rdlock(&board->state_lock);
    int n = board->width * board->height;
    
    // Safety check
    if (n <= 0 || !board->board || !board->pacmans) {
        pthread_rwlock_unlock(&board->state_lock);
        return -1;
    }
    
    char *buf = malloc((size_t)n);
    if (!buf) {
        pthread_rwlock_unlock(&board->state_lock);
        return -1;
    }
    
    for (int i = 0; i < n; i++) {
        // Dados do tabuleiro (converter para formato do cliente)
        char ch = board->board[i].content;
        
        // Converter caracteres para o formato de display
        switch (ch) {
            case 'W': // Wall
                buf[i] = '#';
                break;
            case 'P': // Pacman
                buf[i] = 'C';
                break;
            case 'M': // Ghost
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
    int points = (board->n_pacmans > 0) ? board->pacmans[0].points : 0;
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

    // Envia uma atualização final com o estado final do jogo
    pthread_mutex_lock(&sess->lock);
    int should_send_final = !sess->disconnected;
    pthread_mutex_unlock(&sess->lock);
    
    if (should_send_final) {
        (void)send_board_update(sess);
    }
    return NULL;
}

static void* manager_thread(void *arg) {
    manager_thread_arg_t *mgr_arg = (manager_thread_arg_t*) arg;
    int *register_fd = mgr_arg->register_fd;
    session_t *sessions = mgr_arg->sessions;
    int max_games = mgr_arg->max_games;
    
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    
    while (1) {
        if (got_sigusr1) {
            debug("SIGUSR1 received, dumping top 5 players...\n");
            got_sigusr1 = 0;
            dump_top5(sessions, max_games);
        }
        client_con_req_t con_req;
        memset(&con_req, 0, sizeof(con_req));

        // le o fd_registo para novas sessões
        unsigned char op = 0; 
                
        // Ler OP code
        if (read_full_host(*register_fd, &op, 1, sessions, max_games) != 1) {
            debug("Failed to read op code in manager_thread\n");
            break;
        }
        
        if (op != OP_CODE_CONNECT) {
            debug("Invalid op code in manager_thread: %d\n", op);
            continue;
        }
        
        // Ler caminhos dos FIFOs
        if (read_full_host(*register_fd, con_req.req_pipe_path, MAX_PIPE_PATH_LENGTH, sessions, max_games) != 1 ||
            read_full_host(*register_fd, con_req.notif_pipe_path, MAX_PIPE_PATH_LENGTH, sessions, max_games) != 1) {
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
    bool pending_unload = false; 
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

        if (pending_unload) {
            unload_level(game_board);
            pending_unload = false;
        }

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

                int result = QUIT_GAME;
                if (retval) {
                    result = *retval;
                    free(retval);
                } else {
                    debug("Pacman thread returned NULL\n");
                }

                pthread_mutex_lock(&sess->lock);
                if (result == NEXT_LEVEL) {
                    sess->victory = 0;
                    sess->game_over = 0;
                } else if (result == QUIT_GAME) {
                    sess->game_over = 1;
                    sess->victory = 0;
                }
                pthread_mutex_unlock(&sess->lock);

                (void)send_board_update(sess);

                // Stop threads
                pthread_mutex_lock(&sess->lock);
                sess->shutdown = 1;
                pthread_mutex_unlock(&sess->lock);

                for (int i = 0; i < game_board->n_ghosts; i++) {
                    pthread_join(ghost_tids[i], NULL);
                }
                pthread_join(send_update_tid, NULL);

                free(ghost_tids);

                if(result == NEXT_LEVEL) {
                    pending_unload = true;
                    accumulated_points = sess->board.pacmans[0].points;
                    break;
                }

                if(result == QUIT_GAME) {
                    unload_level(game_board);
                    end_game = true;
                    break;
                }

                accumulated_points = sess->board.pacmans[0].points;      
            }
        }
    }  
    if (!end_game && pending_unload) {
        pthread_mutex_lock(&sess->lock);
        sess->victory = 1;     // vitória final
        sess->game_over = 0;
        pthread_mutex_unlock(&sess->lock);

        (void)send_board_update(sess);
        unload_level(game_board);
        pending_unload = false;
    }
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

        int client_id = exctract_client_id(con_req.req_pipe_path);
        pthread_mutex_lock(&sess->lock);
        sess->client_id = client_id;
        pthread_mutex_unlock(&sess->lock);

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

        // enviar resposta de connect
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
    signal(SIGPIPE, SIG_IGN); // ignorar SIGPIPE

    if (argc != 4) {
        printf("Usage: %s <level_dir> <max_games> <FIFO_name>\n", argv[0]);
        return -1;
    }

    // Abrir arquivo de debug
    open_debug_file("debug.log");
    debug("Servidor iniciado...\n");

    const char *level_dir = argv[1];
    int max_games = atoi(argv[2]);
    const char *register_pipe = argv[3];
    

    if (mkfifo(register_pipe, 0666) < 0){
        if (errno != EEXIST) { 
            perror("mkfifo\n");
            exit(1);
        }
    }
    debug("FIFO de registo criado: %s\n", register_pipe);

    int register_fd = open(register_pipe, O_RDONLY);
    int reg_wr_dummy = open(register_pipe, O_WRONLY | O_NONBLOCK); // deixar register_pipe aberto para sempre
    if (register_fd < 0) {
        perror("open register_pipe\n");
        close_debug_file();
        exit(1);
    }

    queue_init(&queue);

    //instalar handler para SIGUSR1
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    // bloquear SIGUSR1 nas threads filhas
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    // alocar sessions
    session_t *sessions = calloc((size_t)max_games, sizeof(session_t));

    // manager thread
    pthread_t manager_tid;
    manager_thread_arg_t manager_arg;
    manager_arg.register_fd = &register_fd;
    manager_arg.sessions = sessions;
    manager_arg.max_games = max_games;
    pthread_create(&manager_tid, NULL, manager_thread, (void*)&manager_arg);

    // sessions threads
    pthread_t *session_tid = malloc(max_games * sizeof(pthread_t));
    session_thread_arg_t *session_args = malloc(max_games * sizeof(session_thread_arg_t));

    for (int i = 0; i < max_games; i++) {
        session_args[i].session = &sessions[i];
        session_args[i].level_dir = level_dir;
        pthread_create(&session_tid[i], NULL, session_thread, (void*)&session_args[i]);
    }

    // esperar threads terminarem (nunca acontece)
    pthread_join(manager_tid, NULL);

    for (int i = 0; i < max_games; i++) {
        pthread_join(session_tid[i], NULL);
    }

    // cleanup
    close(register_fd);
    if (reg_wr_dummy >= 0) close(reg_wr_dummy);

    free(session_tid);
    free(session_args);
    free(sessions);

    close_debug_file();

    return 0;

}