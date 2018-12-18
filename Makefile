utf8: main.c naive.c range-neon.c range-neon.S
	gcc -g -O3 -Wall -march=native -o $@ $^

.PHONY: clean
clean:
	rm -f utf8 *.o
