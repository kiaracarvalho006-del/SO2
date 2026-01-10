#include "api.h"
#include "protocol.h"
#include "display.h"
#include "debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>

Board board;
bool stop_execution = false;
int tempo;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond_var = PTHREAD_COND_INITIALIZER;
bool board_updated = false;

static void *receiver_thread(void *arg) {
    (void)arg;

    while (true) {
        
        Board new_board = receive_board_update();

        if (!new_board.data) {
            debug("EOF received, stopping execution\n");
            // EOF - fazer draw com a global que tem o último estado válido
            pthread_mutex_lock(&mutex);
            draw_board_client(board);
            refresh_screen();
            stop_execution = true;
            pthread_cond_broadcast(&cond_var);
            pthread_mutex_unlock(&mutex);
            break;
        }

        // Novo board válido - atualizar a global
        pthread_mutex_lock(&mutex);
        if (board.data) free(board.data);
        board = new_board;
        tempo = new_board.tempo;
        board_updated = true;
        pthread_cond_broadcast(&cond_var);
        pthread_mutex_unlock(&mutex);

        draw_board_client(board);
        refresh_screen();

        if (new_board.game_over == 1 || new_board.victory == 1) {
            debug("Game ended (game_over=%d, victory=%d)\n", new_board.game_over, new_board.victory);
            draw_board_client(board);
            refresh_screen();
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            pthread_cond_broadcast(&cond_var);
            pthread_mutex_unlock(&mutex);
            break;
        }
    }

    debug("Returning receiver thread...\n");
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr,
            "Usage: %s <client_id> <register_pipe> [commands_file]\n",
            argv[0]);
        return 1;
    }

    const char *client_id = argv[1];
    const char *register_pipe = argv[2];
    const char *commands_file = (argc == 4) ? argv[3] : NULL;

    FILE *cmd_fp = NULL;
    if (commands_file) {
        cmd_fp = fopen(commands_file, "r");
        if (!cmd_fp) {
            perror("Failed to open commands file");
            return 1;
        }
    }

    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];

    snprintf(req_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_request", client_id);         //TODO: buscar client id no server para o top 5

    snprintf(notif_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_notification", client_id);

    open_debug_file("client-debug.log");

    if (pacman_connect(req_pipe_path, notif_pipe_path, register_pipe) != 0) {
        perror("Failed to connect to server");
        return 1;
    }

    pthread_t receiver_thread_id;
    pthread_create(&receiver_thread_id, NULL, receiver_thread, NULL);

    terminal_init();
    set_timeout(500);

    pthread_mutex_lock(&mutex);
    while (!board_updated && !stop_execution) {
        pthread_cond_wait(&cond_var, &mutex);
    }
    pthread_mutex_unlock(&mutex);

    draw_board_client(board);
    refresh_screen();

    char command;
    int ch;

    while (1) {

        pthread_mutex_lock(&mutex);
        if (stop_execution){
            pthread_mutex_unlock(&mutex);
            debug("Main thread detected game end\n");
            break;
        }
        pthread_mutex_unlock(&mutex);

        if (cmd_fp) {
            // Input from file
            ch = fgetc(cmd_fp);

            if (ch == EOF) {
                // Restart at the start of the file
                rewind(cmd_fp);
                continue;
            }

            command = (char)ch;

            if (command == '\n' || command == '\r' || command == '\0')
                continue;

            command = toupper(command);
            
            // Wait for tempo, to not overflow pipe with requests
            pthread_mutex_lock(&mutex);
            int wait_for = tempo;
            int should_stop = stop_execution;
            pthread_mutex_unlock(&mutex);

            if (should_stop) break;

            //sleep_ms(wait_for);

            int slept = 0;
            while (slept < wait_for) {
                pthread_mutex_lock(&mutex);
                bool stop = stop_execution;
                pthread_mutex_unlock(&mutex);
                if (stop) break;

                int chunk = 10;
                if (wait_for - slept < chunk) chunk = wait_for - slept;
                sleep_ms(chunk);
                slept += chunk;
            }

            pthread_mutex_lock(&mutex);
            should_stop = stop_execution;
            pthread_mutex_unlock(&mutex);
            if (should_stop) break;
            
        } else {
            // Interactive input
            command = get_input();
            command = toupper(command);
        }

        if (command == '\0')
            continue;

        if (command == 'Q') {
            debug("Client pressed 'Q', quitting game\n");
            pacman_disconnect();
            break;
        }

        debug("Command: %c\n", command);

        pacman_play(command);

    }

    debug("Game ended, waiting 2 seconds before cleanup...\n");
    sleep_ms(board.tempo);

    pthread_join(receiver_thread_id, NULL);

    terminal_cleanup();

    pacman_disconnect();

    draw_board_client(board);

    pthread_mutex_lock(&mutex);
    if (board.data) free(board.data);
    board.data = NULL;
    pthread_mutex_unlock(&mutex);

    if (cmd_fp)
        fclose(cmd_fp);

    pthread_mutex_destroy(&mutex);

    return 0;
}
