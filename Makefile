CC = gcc

ifndef nozlib
ZLIB_FLAGS = -DHAVE_ZLIB=1
ZLIB_LIBS  = -lz
else
ZLIB_FLAGS =
ZLIB_LIBS  =
endif

all: disk_hammer

disk_hammer: disk_hammer.o
	$(CC) $^ $(ZLIB_LIBS) -o $@

disk_hammer.o: disk_hammer.c
	$(CC) $(ZLIB_FLAGS) $(CFLAGS) -c -o $@ $<

tags:
	ctags -R

clean:
	rm -f disk_hammer
	rm -f disk_hammer.o
	rm -f tags

.PHONY: tags clean all
