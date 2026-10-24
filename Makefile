CC ?= gcc
CFLAGS ?= -O3 -Wall
LIBS = -lsystemd -lpng

all: fast-crop

fast-crop: main.c
	$(CC) $(CFLAGS) $< $(LIBS) -o $@

install: fast-crop
	install -d $(HOME)/bin
	install -m 755 fast-crop $(HOME)/bin/fast-crop

clean:
	rm -f fast-crop
