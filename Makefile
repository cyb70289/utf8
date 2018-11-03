CC = gcc
CXX = g++
CPPFLAGS = -g -O2 -Wall -march=native

OBJS = main.o naive.o lookup.o lemire.o

utf8: ${OBJS}
	gcc $^ -o $@

utf8-boost: CFLAGS += -DBOOST
utf8-boost: ${OBJS} boost.o
	g++ $^ -o $@

lemire: CFLAGS += -DDEBUG

.PHONY: clean
clean:
	rm -f utf8 utf8-boost lemire *.o
