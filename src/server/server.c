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
    // Código do servidor
    debug("Servidor iniciado...\n");
    // Implementação do servidor aqui

    if (argc != 3) {
        printf("Usage: %s <level_dir> <max_games> <FIFO_name>\n", argv[0]);
        return -1;
    }

    

    return 0;
}