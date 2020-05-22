#
# hl2_tcp makefile
#

OS := $(shell uname)

ifeq ($(OS), Darwin)
	CC  = clang
	LL  = -lm
else
ifeq ($(OS), Linux)
	CC  = cc
	LL = -lm
	STD = -std=c99
else
	$(error OS not detected)
endif
endif

FILES  =  hl2_tcp.c

hl2_tcp:        $(FILES)
		$(CC) $(FILES) $(LL) -lpthread -Os -o hl2_tcp
clean:
	rm hl2_tcp

