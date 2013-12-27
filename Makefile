# Baby's first makefile...
# (created manually for the experience)
# I followed this tutorial: http://mrbook.org/tutorials/make/

CC=gcc
CFLAGS=-c -Wall -fPIC -DPIC -O2 # -fPIC and -DPIC are required, because a LADSPA plugin is a shared library and therefore must be relocatable.
LD=ld
LDFLAGS=-shared
INSTALL=/usr/lib/ladspa/

all: quantizer

quantizer: quantizer.o
	$(LD) $(LDFLAGS) -o quantizer_1337.so quantizer.o

quantizer.o: quantizer.c
	$(CC) $(CFLAGS) -o quantizer.o quantizer.c

cleanup:
	rm quantizer.o

install: quantizer
	cp quantizer1337.so $(INSTALL)
