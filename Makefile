CC      := clang
CFLAGS  := -O3 -Wall -Wextra -std=c11 -pthread

all: xenomem

xenomem: xenomem.c demo.c xenomem.h
	$(CC) $(CFLAGS) xenomem.c demo.c -o xenomem

clean:
	rm -f xenomem
