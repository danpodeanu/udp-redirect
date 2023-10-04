IDIR=include
CC=gcc
CFLAGS=-I$(IDIR) -ggdb -Wall -O3

ODIR=obj

LIBS=

_DEPS =
DEPS = $(patsubst %,$(IDIR)/%,$(_DEPS))

_OBJ = udp-redirect.o
OBJ = $(patsubst %,$(ODIR)/%,$(_OBJ))

$(ODIR)/%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

udp-redirect: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

.PHONY: clean

clean:
	rm -f udp-redirect $(ODIR)/*.o *~ core
