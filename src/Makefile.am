#  $Id$
AUTOMAKE_OPTIONS=foreign

include $(top_srcdir)/rtems-pre.am

EXEEXT=$(OBJEXEEXT)

libdirutils_a_SOURCES          = dirutils.c

lib_LIBRARIES                  = libdirutils.a

if HAVE_CEXP
bin_PROGRAMS                   = dirutils

dirutils_SOURCES               =
dirutils_LDADD                 = dirutils.$(OBJEXT)
dirutils_DEPENDENCIES          = $(dirutils_LDADD)
dirutils_LINK                  = $(OBJLINK)
endif

if BUILD_RTEMS_NFS

librtemsNfs_a_SOURCES          = rpcio.c nfs.c sock_mbuf.c xdr_mbuf.c
EXTRA_librtemsNfs_a_SOURCES    = rpcio.modini.c nfs.modini.c cexphelp.c

lib_LIBRARIES                 += librtemsNfs.a

include_HEADERS                = librtemsNfs.h rpcio.h

if HAVE_CEXP
librtemsNfs_a_LIBADD           = cexphelp.$(OBJEXT)
librtemsNfs_a_DEPENDENCIES     = cexphelp.$(OBJEXT)

bin_PROGRAMS                  += rpcio nfs

rpcio_SOURCES                  = rpcio.modini.c
rpcio_LDADD                    = rpcio.$(OBJEXT)
rpcio_DEPENDENCIES             = $(rpcio_LDADD)
rpcio_LINK                     = $(OBJLINK)

nfs_SOURCES                    = rpcio.modini.c
nfs_LDADD                      = nfs.$(OBJEXT) sock_mbuf.$(OBJEXT)
nfs_LDADD                     += xdr_mbuf.$(OBJEXT)
nfs_DEPENDENCIES               = $(nfs_LDADD)
nfs_LINK                       = $(OBJLINK)

endif

AM_CPPFLAGS                   += -I$(srcdir)/../proto

endif

include $(top_srcdir)/rtems.am
