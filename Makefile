ifeq ($(PREFIX),)
    PREFIX := /usr/local
endif

CC=gcc
CFLAGS=-Wall -O3

ODIR=obj
IDIR=include

_OBJ = udp-redirect.o
_HEADER =

OBJ = $(patsubst %,$(ODIR)/%,$(_OBJ))
HEADER = $(patsubst %,$(IDIR)/%,$(_HEADER))

$(ODIR)/%.o: %.c $(HEADER)
	$(CC) -c -o $@ $< $(CFLAGS)

udp-redirect: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS)

install: udp-redirect
	install -d $(DESTDIR)$(PREFIX)/bin/
	install -m 755 udp-redirect $(DESTDIR)$(PREFIX)/bin/
	install -d $(DESTDIR)$(PREFIX)/share/man/man1/
	install -m 644 udp-redirect.1 $(DESTDIR)$(PREFIX)/share/man/man1/

.PHONY: clean

clean:
	rm -f udp-redirect $(ODIR)/*.o *~ core
	rm -fr docs/

docs:
	doxygen
