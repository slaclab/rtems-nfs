#INCS = -I. -I/afs/slac/package/rtems/prod/rtems/powerpc-rtems/svgm/lib/include
INCS = -I. -I./proto/
#DEFS = -DNFS
CFLAGS = -O2 $(DEFS) $(INCS) -g
#CC = powerpc-rtems-gcc
LDFLAGS = -L./proto

#all: mount_nfs.o

m: mnt.o rpcio.o
	$(CC) -o $@ $^ $(LDFLAGS) -lnfsprot

%.o: %.c
	$(CC) -c -o $@ $(CFLAGS) $^

clean:
	$(RM) *.o
