CC = gcc
CXX = g++
CPPFLAGS = -g -O3 -Wall -march=native

OBJS = main.o naive.o lookup.o lemire-sse.o lemire-neon.o \
	   range-sse.o range-neon.o

utf8: ${OBJS}
	gcc $^ -o $@

utf8-boost: CFLAGS += -DBOOST
utf8-boost: ${OBJS} boost.o
	g++ $^ -o $@

lemire-sse: CFLAGS += -DDEBUG
lemire-neon: CFLAGS += -DDEBUG

range: CFLAGS += -DDEBUG
range: range-sse.o range-neon.o naive.o
	gcc $^ -o $@

.PHONY: clean
clean:
	rm -f utf8 utf8-boost lemire-neon lemire-sse range *.o
