ifdef RTEMS
INCS = -I/afs/slac/package/rtems/prod/rtems/powerpc-rtems/svgm/lib/include
CC   = powerpc-rtems-gcc
endif
CFLAGS = -O $(INCS)
RANLIB = ranlib

XFILES = $(wildcard *.x)

all: libnfsprot.a

libnfsprot.a:	nfs_prot_xdr.o mount_prot_xdr.o
	$(AR) cr $@ $^
	$(RANLIB)	$@

%_xdr.c: %.x %.h
	rpcgen -c $< > $@

%.h: %.x
	rpcgen -h $< > $@

%.o: %.c
	$(CC) -c -o $@ $(CFLAGS) $^

.PRECIOUS: $(XFILES:%.x=%.h) $(XFILES:%.x=%_xdr.c)

clean:
	$(RM) libnfsprot.a *.o

distclean: clean
	$(RM) $(XFILES:%.x=%*.c) $(XFILES:%.x=%*.h)
