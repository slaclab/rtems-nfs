#include <rpc/rpc.h>
#include <rpc/pmap_prot.h>
#include <nfs_prot.h>
#include <mount_prot.h>
#include <stdio.h>
#include "rpcio.h"
#include <string.h>
#include <assert.h>

#define NFS_VERSION_2 NFS_VERSION

#define DEBUG
#undef  DEBUG_CLNT

#define NFS_RETRY_PERIOD_S	3
#define MNT_RETRY_PERIOD_S	3

#define WARN(args...)	do { fprintf(stderr,args); fputc('\n',stderr); } while (0)

#ifdef DEBUG_CLNT
#define RpcUdpClnt 			CLIENT*
#define rpcUdpClntCall		clnt_call
#define rpcUdpClntDestroy	clnt_destroy
#endif


static RpcUdpClnt
clntCreate(struct sockaddr_in *psa, int prog, int vers, struct timeval t)
{
int					port;

	if (!psa->sin_port) {
		psa->sin_port = htons(PMAPPORT);
		port          = pmap_getport(psa, prog, vers ,IPPROTO_UDP);
		psa->sin_port = htons(port);
	}
	if (0==psa->sin_port)
		return 0;

#ifdef DEBUG_CLNT
	printf("%s: %i\n",inet_ntoa(psa->sin_addr), ntohs(psa->sin_port));
	return clnt_create(inet_ntoa(psa->sin_addr), prog, vers, "UDP");
#else
	return rpcUdpClntCreate(psa, prog, vers, t);
#endif
}

static RpcUdpClnt
clntCreateNfs2(struct sockaddr_in *psa)
{
struct timeval t;
	t.tv_sec  = NFS_RETRY_PERIOD_S;
	t.tv_usec = 0;
#ifdef NFS_V2_PORT
	if (0 == psa->sin_port)
		psa->sin_port = htons(NFS_V2_PORT);
#endif
	return clntCreate(psa, NFS_PROGRAM, NFS_VERSION_2, t);
}

static RpcUdpClnt
clntCreateMnt1(struct sockaddr_in *psa)
{
struct timeval t;
	t.tv_sec  = MNT_RETRY_PERIOD_S;
	t.tv_usec = 0;
#ifdef MOUNT_V1_PORT
	if (0 == psa->sin_port)
		psa->sin_port = htons(MOUNT_V1_PORT);
#endif
	return clntCreate(psa, MOUNT_PROGRAM, MOUNT_V1, t);
}

#ifdef DEBUG
diropargs			dbg_diropargs;
struct sockaddr_in	dbg_server;
#endif

int
nfsMount(char *host, dirpath path, char *mntpt)
{
RpcUdpClnt			clp;
struct sockaddr_in	saddr;
struct timeval		timeout;
enum clnt_stat		stat;
struct fhstatus		fhstat;

	if ( ! inet_aton(host, &saddr.sin_addr) ) {
			WARN("Bad host address '%s'", host);
			return -1;
	}
	saddr.sin_family = AF_INET;
	saddr.sin_port   = 0;

	clp = clntCreateNfs2(&saddr);

	if ( !clp ) {
			WARN("unable to create NFS2 client for PING - invalid server port?");
			return -1;
	}

#ifdef DEBUG
	dbg_server = saddr;
#endif

	timeout.tv_sec  = 10;
	timeout.tv_usec = 0;
	stat = rpcUdpClntCall(clp, NFSPROC_NULL, xdr_void, 0, xdr_void, 0, timeout);

	rpcUdpClntDestroy(clp);

	switch (stat) {
		case RPC_SUCCESS:
			break;

		case RPC_PROGVERSMISMATCH:
			WARN("Version not supported by server");
			return -1;
		default:
			WARN("Bad NFS RPC");
			return -1;
	}

	saddr.sin_port = 0;
	clp = clntCreateMnt1(&saddr);

	if ( !clp ) {
			WARN("unable to create MOUNT_V1 client");
			return -1;
	}

#ifdef DEBUG_CLNT
	clp->cl_auth = authunix_create_default();
#endif
	timeout.tv_sec  = 10;

	stat = rpcUdpClntCall(clp, MOUNTPROC_MNT, xdr_dirpath, &path, xdr_fhstatus, &fhstat, timeout);

	switch (stat) {
		case RPC_SUCCESS:
			break;
		case RPC_PROGVERSMISMATCH:
			WARN("Version not supported by server");
			return -1;
		default:
			WARN("Bad MNT RPC");
			return -1;
	}

#ifdef DEBUG_CLNT
	auth_destroy(clp->cl_auth);
#endif
	rpcUdpClntDestroy(clp);

	if (fhstat.fhs_status) {
		WARN("Unable to mount '%s': %s\n", path, strerror(fhstat.fhs_status));
	}

#ifdef DEBUG
	else {
		extern void *dbgFhP;

		assert( sizeof(dbg_diropargs.dir) ==
				sizeof(fhstat.fhstatus_u.fhs_fhandle) );
		memcpy(
			&dbg_diropargs.dir,
			fhstat.fhstatus_u.fhs_fhandle,
			sizeof(dbg_diropargs.dir));
		memcpy(
			dbgFhP,
			fhstat.fhstatus_u.fhs_fhandle,
			sizeof(dbg_diropargs.dir));
	}
#endif


	return 0;
}

#ifdef DEBUG
lk(char *name)
{
RpcUdpClnt			clp;
diropres			res;
struct timeval		timeout;
fattr				*f;
enum clnt_stat		stat;


	dbg_diropargs.name=name;
	clp = clntCreateNfs2(&dbg_server);
	if ( !clp ) {
			WARN("unable to create client\n");
			return -1;
	}
	timeout.tv_sec  = 10;
	timeout.tv_usec = 0;
	stat = rpcUdpClntCall(
					clp, NFSPROC_LOOKUP,
					xdr_diropargs, &dbg_diropargs,
					xdr_diropres,  &res,
					timeout
					);
	if (RPC_SUCCESS != stat) {
		fprintf(stderr,"RPC Error %i\n", stat);
		goto cleanup;
	}

	if (res.status) {
		fprintf(stderr,"NFS Error %i (?%s?)\n", res.status, strerror(res.status));
		goto cleanup;
	}
	f = &res.diropres_u.diropres.attributes;
	fprintf(stderr,"type   %i\n", f->type);
	fprintf(stderr,"mode   %i\n", f->mode);
	fprintf(stderr,"nlink  %i\n", f->nlink);
	fprintf(stderr,"fileid %i\n", f->fileid);
	fprintf(stderr,"uid    %i\n", f->uid);
	if (NFDIR == f->type)
		memcpy( &dbg_diropargs.dir,
				&res.diropres_u.diropres.file,
				sizeof(dbg_diropargs.dir));

cleanup:
	xdr_free(xdr_diropres,(void*)&res);
	rpcUdpClntDestroy(clp);

}
#endif

#ifndef __rtems
int
main(int argc, char **argv)
{
char *delimp;
char *buf=0;

	rpcUdpInit();

	if (argc<2 || !(buf=strdup(argv[1])) || !(delimp=strchr(buf,':')) ) {
		WARN("usage: %s host:path",argv[0]);
		free(buf);
		exit(1);
	}
	*delimp++=0;

	if (0==nfsMount(buf, delimp, 0))
		lk(".");

	free(buf);
	return 0;
}
#endif
