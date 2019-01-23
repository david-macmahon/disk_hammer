all: disk_hammer

disk_hammer: disk_hammer.c

tags:
	ctags -R

clean:
	rm -f disk_hammer
	rm -f tags

.PHONY: tags clean all
