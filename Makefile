CFLAGS ?= -O2 -g -Wall -Wextra
LDLIBS = -pthread

memqos: memqos.c
	$(CC) $(CFLAGS) -pthread -o $@ $< $(LDLIBS)

clean:
	rm -f memqos
