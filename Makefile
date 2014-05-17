CC=gcc
CFLAGS=-c -Wall -fPIC -DPIC -O2 -std=c99 # -fPIC and -DPIC are required because a LADSPA plugin is a shared library and must be relocatable.
LD=ld
LDFLAGS=-shared
INSTALL=/usr/lib/ladspa/

all: bitcrusher

bitcrusher: bitcrusher.o
	$(LD) $(LDFLAGS) -o bitcrusher.so bitcrusher.o

bitcrusher.o: bitcrusher.c
	$(CC) $(CFLAGS) -o bitcrusher.o bitcrusher.c

cleanup:
	rm bitcrusher.o

install: all
	mv bitcrusher.so $(INSTALL)
	rm bitcrusher.o
