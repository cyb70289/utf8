CC = gcc
CXX = g++
CPPFLAGS = -g -O2 -Wall -march=native

utf8: main.o naive.o lookup.o
	gcc $^ -o $@

utf8-boost: CFLAGS += -DBOOST
utf8-boost: main.o naive.o lookup.o boost.o
	g++ $^ -o $@

.PHONY: clean
clean:
	rm -f utf8 utf8-boost *.o
