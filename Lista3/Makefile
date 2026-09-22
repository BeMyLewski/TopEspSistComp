CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c99

TARGETS = przombies mkzombies

.PHONY: all clean

all: $(TARGETS)

przombies: przombies.c
	$(CC) $(CFLAGS) -o $@ $<

mkzombies: mkzombies.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS) *.o *.log
