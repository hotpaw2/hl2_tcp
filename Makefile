#
# hl2_tcp makefile
#

OS := $(shell uname)

ifeq ($(OS), Darwin)
	CC  = clang
	LL  = -lm -lpthread
else
ifeq ($(OS), Linux)
	CC  = cc
	LL = -lm -lpthread
	STD = -std=c99
else
	$(error OS not detected)
endif
endif

FLAGS = -Os
# FLAGS = -g

FILES_2  =  hl2_tcp.c hl2_tx.c

all:	hl2_tcp 

hl2_tcp:	$(FILES_2)
	$(CC) $(FILES_2) $(LL) -lpthread -Os -o hl2_tcp

clean:
	rm -f hl2_tcp 

#
