utf8: main.c naive.c
	gcc -g -O2 -Wall -march=native $^ -o $@

.PHONY: clean
clean:
	rm -f utf8
