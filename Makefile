CC      := clang
CFLAGS  := -O3 -Wall -Wextra -std=c11 -pthread

all: xenomem xenomem_stress xenomem_prove

xenomem: xenomem.c demo.c xenomem.h
	$(CC) $(CFLAGS) xenomem.c demo.c -o xenomem

xenomem_stress: xenomem.c stress.c xenomem.h
	$(CC) $(CFLAGS) xenomem.c stress.c -o xenomem_stress

xenomem_prove: xenomem.c prove.c xenomem.h
	$(CC) $(CFLAGS) xenomem.c prove.c -o xenomem_prove

stress: xenomem_stress
	./xenomem_stress

prove: xenomem_prove
	./xenomem_prove

clean:
	rm -f xenomem xenomem_stress xenomem_prove stress_test
