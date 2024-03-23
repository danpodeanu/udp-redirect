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

.PHONY: clean

clean:
	rm -f udp-redirect $(ODIR)/*.o *~ core
	rm -fr docs/

docs:
	doxygen
