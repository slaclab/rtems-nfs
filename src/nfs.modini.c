#include "librtemsNfs.h"

/* CEXP dynamic loader support */

void
_cexpModuleInitialize(void *mod)
{	
	nfsInit(0,0);
}

int
_cexpModuleFinalize(void *mod)
{	
	return nfsCleanup();
}


