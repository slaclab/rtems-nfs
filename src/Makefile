#
#  $Id$
#
# Templates/Makefile.leaf
# 	Template leaf node Makefile
#

# C source names, if any, go here -- minus the .c
C_PIECES=rpcio nfs
C_FILES=$(C_PIECES:%=%.c)
C_O_FILES=$(C_PIECES:%=${ARCH}/%.o)

# C++ source names, if any, go here -- minus the .cc
CC_PIECES=
CC_FILES=$(CC_PIECES:%=%.cc)
CC_O_FILES=$(CC_PIECES:%=${ARCH}/%.o)

H_FILES=

# Assembly source names, if any, go here -- minus the .S
S_PIECES=
S_FILES=$(S_PIECES:%=%.S)
S_O_FILES=$(S_FILES:%.S=${ARCH}/%.o)

SRCS=$(C_FILES) $(CC_FILES) $(H_FILES) $(S_FILES)
OBJS=$(C_O_FILES) $(CC_O_FILES) $(S_O_FILES)

PGMS=${ARCH}/nfs.obj ${ARCH}/rpcio.obj

include $(RTEMS_MAKEFILE_PATH)/Makefile.inc
include $(RTEMS_CUSTOM)
include $(RTEMS_ROOT)/make/leaf.cfg

#
# (OPTIONAL) Add local stuff here using +=
#

DEFINES  +=

CPPFLAGS += -I. -I./proto
CFLAGS   += -DHAVE_CEXP

#
# CFLAGS_DEBUG_V are used when the `make debug' target is built.
# To link your application with the non-optimized RTEMS routines,
# uncomment the following line:
# CFLAGS_DEBUG_V += -qrtems_debug
#

LDFLAGS   += -L./proto/${ARCH} -lnfsprot

#
# Add your list of files to delete here.  The config files
#  already know how to delete some stuff, so you may want
#  to just run 'make clean' first to see what gets missed.
#  'make clobber' already includes 'make clean'
#

#CLEAN_ADDITIONS += xxx-your-debris-goes-here
CLOBBER_ADDITIONS +=

ifndef INSTALLDIR
INSTALLDIR = $(PROJECT_RELEASE)
endif

all:	${ARCH} $(SRCS) $(PGMS)

$(filter %.obj, $(PGMS)): ${OBJS}
	$(LD_FOR_TARGET) -r -o $@ $(OBJS)
	

#	$(LD_FOR_TARGET) $(AM_CFLAGS) $(AM_LDFLAGS) -r -o $@ $(LINK_OBJS) $(LINK_LIBS)

$(filter %.exe, $(PGMS)): ${OBJS} ${LINK_FILES}
	$(make-exe)
	xsyms $@ $(@:%.exe=%.sym)

# Install the program(s), appending _g or _p as appropriate.
# for include files, just use $(INSTALL_CHANGE)
install:  all
	$(INSTALL_VARIANT) -m 555 ${PGMS} ${INSTALLDIR}/bin
	$(INSTALL_VARIANT) -m 555 ${PGMS} ${INSTALLDIR}/bin

balla:
	echo $(CPPFLAGS)
