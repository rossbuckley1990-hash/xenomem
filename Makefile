CC      := clang
CFLAGS  := -O3 -Wall -Wextra -std=c11 -pthread

all: xenomem xenomem_stress

xenomem: xenomem.c demo.c xenomem.h
	$(CC) $(CFLAGS) xenomem.c demo.c -o xenomem

xenomem_stress: xenomem.c stress.c xenomem.h
	$(CC) $(CFLAGS) xenomem.c stress.c -o xenomem_stress

stress: xenomem_stress
	./xenomem_stress

clean:
	rm -f xenomem xenomem_stress stress_test
