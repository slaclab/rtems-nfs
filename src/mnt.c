#include <rpc/rpc.h>
#include <rpc/pmap_prot.h>
#include <nfs_prot.h>
#include <mount_prot.h>
#include <stdio.h>
#include "rpcio.h"

#define NFS_VERSION_2 NFS_VERSION

#define NFS_RETRY_PERIOD_S	3
#define MNT_RETRY_PERIOD_S	3

#define WARN(args...)	do { fprintf(stderr,args); fputc('\n',stderr); } while (0)


static RpcUdpClnt
clntCreate(struct sockaddr_in *psa, int prog, int vers, struct timeval t)
{
int					port;

	if (!psa->sin_port) {
		psa->sin_port = htons(PMAPPORT);
		port          = pmap_getport(psa, prog, vers ,IPPROTO_UDP);
		psa->sin_port = htons(port);
	}
	if (0==port)
		return 0;

	return rpcUdpClntCreate(psa, prog, vers, t);
}

static RpcUdpClnt
clntCreateNfs2(struct sockaddr_in *psa)
{
struct timeval t;
	t.tv_sec  = NFS_RETRY_PERIOD_S;
	t.tv_usec = 0;
#ifdef NFS_V2_PORT
	if (0 == psa->sin_port)
		psa->sin_port = NFS_V2_PORT;
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
		psa->sin_port = MOUNT_V1_PORT;
#endif
	return clntCreate(psa, MOUNT_PROGRAM, MOUNT_V1, t);
}


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
			WARN("unable to create NFS2 client for PING");
			return -1;
	}
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

/*	clp->cl_auth = authunix_create_default(); */
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

/*	auth_destroy(clp->cl_auth); */
	rpcUdpClntDestroy(clp);

	if (fhstat.fhs_status) {
		WARN("Unable to mount '%s': %s\n", path, strerror(fhstat.fhs_status));
	}
		

	return 0;
}

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

	nfsMount(buf, delimp, 0);

	free(buf);
	return 0;
}
