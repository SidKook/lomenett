sim: sim.c ../supervisor.h
	$(CC) -Wall -Wextra -O1 -o sim sim.c

run: sim
	./sim

clean:
	rm -f sim

.PHONY: run clean
