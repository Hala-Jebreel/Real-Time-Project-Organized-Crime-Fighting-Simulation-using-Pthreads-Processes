CC = gcc
CFLAGS = -g -O2 -Wall -Wextra -std=gnu11 -pthread
LDFLAGS = -lrt -lm

TARGETS = main gang_process police_process gui

# Pattern rule for object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

all: $(TARGETS)

main: main.o config.o ipc_utils.o json.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

gang_process: gang_process.o config.o ipc_utils.o json.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

police_process: police_process.o config.o ipc_utils.o json.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

gui: gui.o ipc_utils.o config.o json.o
	$(CC) $(CFLAGS) -o $@ $^ -lGL -lGLU -lglut -lm $(LDFLAGS)


clean:
	rm -f $(TARGETS) *.o