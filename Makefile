CC = gcc
CXX = g++
CPPFLAGS = -g -O3 -Wall -march=native
CXXFLAGS = -std=c++11

OBJS = main.o naive.o lookup.o lemire-sse.o lemire-neon.o  \
	   range-sse.o range-neon.o range2-sse.o range2-neon.o \
	   lemire-avx2.o range-avx2.o

utf8: ${OBJS}
	gcc $^ -o $@

utf8-boost: CFLAGS += -DBOOST
utf8-boost: ${OBJS} boost.o
	g++ $^ -o $@

.PHONY: clean
clean:
	rm -f utf8 utf8-boost ascii *.o
