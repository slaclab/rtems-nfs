INCS = -I. -I./proto/
ifdef RTEMS
INCS += -I. -I/afs/slac/package/rtems/prod/rtems/powerpc-rtems/svgm/lib/include
endif
#DEFS = -DNFS
CFLAGS = -O2 $(DEFS) $(INCS) -g
ifdef RTEMS
CC = powerpc-rtems-gcc
LD = powerpc-rtems-ld
TARG = nfs.obj
else
TARG = m
endif
LDFLAGS += -L./proto

all: $(TARG)

m: mnt.o rpcio.o
	$(CC) -o $@ $^ $(LDFLAGS) -lnfsprot

%.o: %.c
	$(CC) -c -o $@ $(CFLAGS) $^

nfs.obj: nfs.o
	$(LD) -o $@ -r $^ -Lproto  -lnfsprot

rpcio.obj: rpcio.o xdr_mbuf.o sock_mbuf.o
	$(LD) -o $@ -r $^

clean:
	$(RM) *.o m *.obj
