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

int main(int arc,char *argv[]) {
    debug("Servidor iniciado...\n");
    // Implementação do servidor aqui

    if (argc != 3) {
        printf("Usage: %s <level_dir> <max_games> <FIFO_name>\n", argv[0]);
        return -1;
    }

    DIR* level_dir = opendir(argv[1]);
        
    if (level_dir == NULL) {
        fprintf(stderr, "Failed to open directory: %s\n", argv[1]);
        return 0;
    }

    int max_games = atoi(argv[2]);
    int current_games = 0;

    open_debug_file("debug.log");

    terminal_init();

    bool end_game = false;
    board_t game_board;
    

    return 0;
}