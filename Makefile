#
#  $Id$
#
REVISION=$(filter-out $$%,$$Name$$)

include $(RTEMS_MAKEFILE_PATH)/Makefile.inc

include $(RTEMS_CUSTOM)
RECURSE_TARGETS += tar-recursive
include $(RTEMS_ROOT)/make/directory.cfg

SUBDIRS=proto src

tar: tar-recursive
	@if [ -z $(REVISION) ] ; then \
        echo "I need a version checked out with a revision tag to make a tarball";\
        exit 1;\
    else \
        echo tarring revision $(REVISION);\
        tar Xcfz tarexcl $(REVISION).tgz -C .. $(shell basename `pwd`) ;\
    fi
