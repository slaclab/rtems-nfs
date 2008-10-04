#  $Id$
AUTOMAKE_OPTIONS=foreign

include $(top_srcdir)/rtems-pre.am

libnfsprot_a_SOURCES    = nfs_prot_xdr.c mount_prot_xdr.c

lib_LIBRARIES           = libnfsprot.a

# inline declarations require -O
# rpcgen produces unused variables
AM_CFLAGS              += -O2 -Winline -Wno-unused-variable

include $(top_srcdir)/rtems.am

XFILES                  = $(wildcard $(srcdir)/*.x)

%_xdr.c: %.x %.h
	rpcgen -c -C $< > $@

%.h: %.x
	rpcgen -h -C $< > $@

.PRECIOUS: $(XFILES:%.x=%.h) $(XFILES:%.x=%_xdr.c)

distclean-local:
	$(RM) $(srcdir)/*.c $(srcdir)/*.h
