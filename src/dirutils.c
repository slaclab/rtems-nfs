/* $Id$ */

/* very crude and basic fs utilities for testing the NFS */

/* Till Straumann, <strauman@slac.stanford.edu>, 10/2002 */

#ifdef __vxworks
#include <vxWorks.h>
#endif
#include <stdio.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdlib.h>

#ifdef HAVE_CEXP
#include <cexpHelp.h>
#endif

#define BUFFERSZ 10000

#ifndef __vxworks
int
pwd(void)
{
char buf[MAXPATHLEN];

	if ( !getcwd(buf,MAXPATHLEN)) {
		perror("getcwd");
		return -1;
	} else {
		printf("%s\n",buf);
	}
	return 0;
}

static int
ls_r(char *path, char *chpt, char *name, struct stat *buf)
{
char *t;
	sprintf(chpt, "/%s", name);
	if (lstat(path,buf)) {
		fprintf(stderr,"stat(%s): %s\n", path, strerror(errno));
		return -1;
	}
	switch ( buf->st_mode & S_IFMT ) {
		case S_IFSOCK:
		case S_IFIFO:	t = "|"; break;

		default:
		case S_IFREG:
		case S_IFBLK:
		case S_IFCHR:
						t = "";  break;
		case S_IFDIR:
						t = "/"; break;
		case S_IFLNK:
						t = "@"; break;
	}

	printf("%10li, %10lib, %5i.%-5i 0%04o %s%s\n",
				buf->st_ino,
				buf->st_size,
				buf->st_uid,
				buf->st_gid,
				buf->st_mode & ~S_IFMT,
				name,
				t);
	*chpt = 0;
	return 0;
}

int
ls(char *dir, char *opts)
{
struct dirent	*de;
char			path[MAXPATHLEN+1];
char			*chpt;
DIR				*dp  = 0;
int				rval = -1;
struct stat		buf;

	if ( !dir )
		dir = ".";

	strncpy(path, dir, MAXPATHLEN);
	path[MAXPATHLEN] = 0;
	chpt = path+strlen(path);

	if ( !(dp=opendir(dir)) ) {
		perror("opendir");
		goto cleanup;
	}

	while ( (de = readdir(dp)) ) {
		ls_r(path, chpt, de->d_name, &buf);
	}

	rval = 0;

cleanup:
	if (dp)
		closedir(dp);
	return rval;
}
#endif

#if 0
		fprintf(stderr, "usage: cp(""from"",[""to""[,""-f""]]\n");
		fprintf(stderr, "          ""to""==NULL -> stdout\n");
		fprintf(stderr, "          ""-f""       -> overwrite existing file\n");
#endif

int
cp(char *from, char *to, char *opts)
{
struct stat	st;
int			got,put,tot;
char		*buf  = 0;
int			rval  = -1;
int			ffd   = -1;
int			tfd   = -1;
int			flags = O_CREAT | O_WRONLY | O_TRUNC | O_EXCL;

	if (from) {

	if ((ffd=open(from,O_RDONLY,0)) < 0) {
		fprintf(stderr,
				"Opening %s for reading: %s\n",
				from,
				strerror(errno));
		goto cleanup;
	}

	if (fstat(ffd, &st)) {
		fprintf(stderr,
				"rstat(%s): %s\n",
				from,
				strerror(errno));
		goto cleanup;
	}


	if (!S_ISREG(st.st_mode)) {
		fprintf(stderr,"Refuse to copy a non-regular file\n");
		errno = EINVAL;
		goto cleanup;
	}

	} else {
		ffd        = fileno(stdin);
		st.st_mode = 0644;
	}

	if (opts && strchr(opts,'f'))
		flags &= ~ O_EXCL;

	if (to) {
		if ((tfd=open(to,flags,st.st_mode)) < 0) {
			fprintf(stderr,
					"Opening %s for writing: %s\n",
					to,
					strerror(errno));
			goto cleanup;
		}
	} else {
		tfd = fileno(stdout);
	}

	if ( !(buf = malloc(BUFFERSZ)) ) {
		fprintf(stderr,"cp: unable to allocate buffer - out of memory\n");
		errno = ENOMEM;
		goto cleanup;
	}

	tot = 0;
	while ( (got=read(ffd,buf,BUFFERSZ)) > 0 ) {
		if (got !=(put=write(tfd,buf,got))) {
			if (put<0) {
				fprintf(stderr,"Write error: %s\n",strerror(errno));
			} else {
				fprintf(stderr,"Write error: unable to write whole block\n");
			}
			goto cleanup;
		}
		tot += got;
	}
	if (got < 0) {
		fprintf(stderr,"Read error: %s\n",strerror(errno));
		goto cleanup;
	}
	rval = 0;

cleanup:
	free(buf);

	if (from && ffd>=0)
		close(ffd);
	if (to && tfd>=0)
		close(tfd);

	return rval;
}

int
ln(char *to, char *name, char *opts)
{
	if (!to) {
		fprintf(stderr,"ln: need 'to' argument\n");
		return -1;
	}
	if (!name) {
		if ( !(name = strrchr(to,'/')) ) {
			fprintf(stderr,
					"ln: 'unable to link %s to %s\n",
					to,to);
			return -1;
		}
		name++;
	}
	if (opts || strchr(opts,'s')) {
		if (symlink(name,to)) {
			fprintf(stderr,"symlink: %s\n",strerror(errno));
			return -1;
		}
	} else {
		if (link(name,to)) {
			fprintf(stderr,"hardlink: %s\n",strerror(errno));
			return -1;
		}
	}
	return 0;
}

int
rm(char *path)
{
	return unlink(path);
}

int
cd(char *path)
{
	return chdir(path);
}

#ifdef HAVE_CEXP
static CexpHelpTabRec _cexpHelpTabDirutils[]={
	HELP(
"copy a file: cp(""from"",[""to""[,""-f""]])\n\
                 from = NULL <-- stdin\n\
                 to   = NULL --> stdout\n\
                 option -f: overwrite existing file\n",
		int,
		cp, (char *from, char *to, char *options)
		),
	HELP(
"list a directory: ls([""dir""])\n",
		int,
		ls, (char *dir)
		),
	HELP(
"remove a file\n",
		int,
		rm, (char *path)
		),
	HELP(
"change the working directory\n",
		int,
		cd, (char *path)
		),
	HELP(
"create a link: ln(""to"",""name"",""[-s]""\n\
                   -s creates a symlink\n",
		int,
		ln, (char *to, char *name, char *options)
		),
	HELP("",,0,)
};
#endif
