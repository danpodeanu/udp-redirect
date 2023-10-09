CC=gcc
CFLAGS=-Wall -O3

ODIR=obj

_OBJ = udp-redirect.o
OBJ = $(patsubst %,$(ODIR)/%,$(_OBJ))

$(ODIR)/%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

udp-redirect: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS)

.PHONY: clean

clean:
	rm -f udp-redirect $(ODIR)/*.o *~ core
	rm -fr docs/

docs:
	doxygen
