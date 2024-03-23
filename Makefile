CC=gcc
CFLAGS=-Wall -O3 -std=c99

ODIR=obj
IDIR=include

_OBJ = settings.o network.o statistics.o udp-redirect.o
_HEADER = statistics.h debug.h settings.h network.h

OBJ = $(patsubst %,$(ODIR)/%,$(_OBJ))
HEADER = $(patsubst %,$(IDIR)/%,$(_HEADER))

$(ODIR)/%.o: %.c $(HEADER)
	$(CC) -c -o $@ $< $(CFLAGS)

udp-redirect: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS)

.PHONY: clean

clean:
	rm -f udp-redirect $(ODIR)/*.o *~ core
	rm -fr docs/

docs:
	doxygen
