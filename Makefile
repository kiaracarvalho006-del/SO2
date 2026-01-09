# Compiler variables
CC = gcc
CFLAGS = -g -Wall -Wextra -Werror -std=c17 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lncurses -pthread

# Directory variables
SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin
INCLUDE_DIR = include

# executables
CLIENT_TARGET = Pacmanist
SERVER_TARGET = PacmanServer

# Common objects
COMMON_OBJS = common.o debug.o

# Client objects
CLIENT_OBJS = client_main.o api.o display.o $(COMMON_OBJS)

# Server objects  
SERVER_OBJS = game.o board.o parser.o display.o $(COMMON_OBJS)

# Dependencies
display.o = display.h
board.o = board.h
parser.o = parser.h

# Object files path
vpath %.o $(OBJ_DIR)
vpath %.c $(SRC_DIR)/client $(SRC_DIR)/server $(SRC_DIR)/common

# Make targets
all: client server

client: $(BIN_DIR)/$(CLIENT_TARGET)

server: $(BIN_DIR)/$(SERVER_TARGET)

$(BIN_DIR)/$(CLIENT_TARGET): $(CLIENT_OBJS) | folders
	$(CC) $(CFLAGS) $(addprefix $(OBJ_DIR)/,$(CLIENT_OBJS)) -o $@ $(LDFLAGS)

$(BIN_DIR)/$(SERVER_TARGET): $(SERVER_OBJS) | folders
	$(CC) $(CFLAGS) $(addprefix $(OBJ_DIR)/,$(SERVER_OBJS)) -o $@ $(LDFLAGS)

# dont include LDFLAGS in the end, to allow compilation on macos
%.o: %.c $($@) | folders
	$(CC) -I $(INCLUDE_DIR) $(CFLAGS) -o $(OBJ_DIR)/$@ -c $<

# run the client
run-client: client
	@./$(BIN_DIR)/$(CLIENT_TARGET) $(ARGS)

# run the server
run-server: server
	@./$(BIN_DIR)/$(SERVER_TARGET) $(ARGS)

# Create folders
folders:
	mkdir -p $(OBJ_DIR)
	mkdir -p $(BIN_DIR)

# Clean object files and executables
clean:
	rm -f $(OBJ_DIR)/*.o
	rm -f $(BIN_DIR)/$(CLIENT_TARGET)
	rm -f $(BIN_DIR)/$(SERVER_TARGET)

# identify targets that do not create files
.PHONY: all clean client server run-client run-server folders
