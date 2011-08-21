VERSION=0.1
DISTNAME=gstfs-$(VERSION)
DISTFILES=*.[ch] *.sh Makefile README COPYING

SRCS=gstfs.c xcode.c
OBJS=$(SRCS:.c=.o)

LIBS=-lpthread `pkg-config --libs fuse glib-2.0 gstreamer-0.10`

CFLAGS+=-g -Wall -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=26 `pkg-config --cflags fuse glib-2.0 gstreamer-0.10`

all: gstfs

gstfs: $(OBJS)
	gcc -o gstfs $(OBJS) $(LIBS)

clean:
	$(RM) gstfs *.o

dist: clean
	mkdir $(DISTNAME)
	cp $(DISTFILES) $(DISTNAME)
	tar czvf $(DISTNAME).tar.gz $(DISTNAME)
	$(RM) -r $(DISTNAME)

distcheck: dist
	mkdir build
	cd build && tar xzvf ../$(DISTNAME).tar.gz && \
	cd $(DISTNAME) && $(MAKE)
	$(RM) -r build
