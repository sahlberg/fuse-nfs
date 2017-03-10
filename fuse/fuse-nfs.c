/*
   Copyright (C) by Ronnie Sahlberg <ronniesahlberg@gmail.com> 2013
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/
/* A FUSE filesystem based on libnfs. */

#define FUSE_USE_VERSION 26
#define _FILE_OFFSET_BITS 64

#include "../config.h"

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/types.h>
#include <nfsc/libnfs.h>

#ifdef WIN32
#include <winsock2.h>
#endif

#ifndef FUSE_STAT
#define FUSE_STAT stat
#endif

#define discard_const(ptr) ((void *)((intptr_t)(ptr)))

struct nfs_context *nfs = NULL;
int custom_uid = -1;
int custom_gid = -1;

uid_t mount_user_uid;
gid_t mount_user_gid;

int fusenfs_allow_other_own_ids=0;
int fuse_default_permissions=1;
int fuse_multithreads=0;

#ifdef __MINGW32__
gid_t getgid(){
	if( custom_gid == -1 )
		return 65534;
	return custom_gid;
}

uid_t getuid(){
	if( custom_uid == -1 )
		return 65534;
	return custom_uid;
}
#endif

static int map_uid(int possible_uid) {
    if (custom_uid != -1 && possible_uid == custom_uid){
        return getuid();
    }
    return possible_uid;
}

static int map_gid(int possible_gid) {
    if (custom_gid != -1 && possible_gid == custom_gid){
        return getgid();
    }
    return possible_gid;
}

/* Update the rpc credentials to the current user unless
 * have are overriding the credentials via url arguments.
 */
static void update_rpc_credentials(void) {
	if ((custom_uid == -1 || fuse_get_context()->uid != mount_user_uid) && fusenfs_allow_other_own_ids)
	{
		nfs_set_uid(nfs, fuse_get_context()->uid);
	}
	else
	{
		nfs_set_uid(nfs, custom_uid);
	}
	if ((custom_gid == -1 || fuse_get_context()->gid != mount_user_gid) && fusenfs_allow_other_own_ids)
	{
		nfs_set_gid(nfs, fuse_get_context()->gid);
	}
	else 
	{
		nfs_set_gid(nfs, custom_gid);
	}
}

static int fuse_nfs_getattr(const char *path, struct FUSE_STAT *stbuf)
{
	int ret = 0;
	struct nfs_stat_64 nfs_st;

	update_rpc_credentials();

	ret = nfs_lstat64(nfs, path, &nfs_st);

	stbuf->st_dev          = nfs_st.nfs_dev;
	stbuf->st_ino          = nfs_st.nfs_ino;
	stbuf->st_mode         = nfs_st.nfs_mode;
	stbuf->st_nlink        = nfs_st.nfs_nlink;
    stbuf->st_uid          = map_uid(nfs_st.nfs_uid);
    stbuf->st_gid          = map_gid(nfs_st.nfs_gid);
	stbuf->st_rdev         = nfs_st.nfs_rdev;
	stbuf->st_size         = nfs_st.nfs_size;
	stbuf->st_blksize      = nfs_st.nfs_blksize;
	stbuf->st_blocks       = nfs_st.nfs_blocks;

#if defined(HAVE_ST_ATIM) || defined(__MINGW32__)
	stbuf->st_atim.tv_sec  = nfs_st.nfs_atime;
	stbuf->st_atim.tv_nsec = nfs_st.nfs_atime_nsec;
	stbuf->st_mtim.tv_sec  = nfs_st.nfs_mtime;
	stbuf->st_mtim.tv_nsec = nfs_st.nfs_mtime_nsec;
	stbuf->st_ctim.tv_sec  = nfs_st.nfs_ctime;
	stbuf->st_ctim.tv_nsec = nfs_st.nfs_ctime_nsec;
#else
	stbuf->st_atime      = nfs_st.nfs_atime;
	stbuf->st_mtime      = nfs_st.nfs_mtime;
	stbuf->st_ctime      = nfs_st.nfs_ctime;
	stbuf->st_atime_nsec = nfs_st.nfs_atime_nsec;
	stbuf->st_mtime_nsec = nfs_st.nfs_mtime_nsec;
	stbuf->st_ctime_nsec = nfs_st.nfs_ctime_nsec;
#endif
	return ret;
}

static int fuse_nfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	struct nfsdir *nfsdir;
	struct nfsdirent *nfsdirent;

	int ret;

        update_rpc_credentials();

	ret = nfs_opendir(nfs, path, &nfsdir);
	if (ret < 0) {
		return ret;
	}
	while ((nfsdirent = nfs_readdir(nfs, nfsdir)) != NULL) {
		filler(buf, nfsdirent->name, NULL, 0);
	}

	nfs_closedir(nfs, nfsdir);
	return 0;
}

static int fuse_nfs_readlink(const char *path, char *buf, size_t size)
{
        update_rpc_credentials();

	return nfs_readlink(nfs, path, buf, size);
}

static int fuse_nfs_open(const char *path, struct fuse_file_info *fi)
{
	int ret = 0;
	struct nfsfh *nfsfh;

        update_rpc_credentials();

	fi->fh = 0;
	ret = nfs_open(nfs, path, fi->flags, &nfsfh);
	if (ret < 0) {
		return ret;
	}

	fi->fh = (uint64_t)nfsfh;

	return ret;
}

static int fuse_nfs_release(const char *path, struct fuse_file_info *fi)
{
	struct nfsfh *nfsfh = (struct nfsfh *)fi->fh;

	nfs_close(nfs, nfsfh);
	return 0;
}

static int fuse_nfs_read(const char *path, char *buf, size_t size,
       off_t offset, struct fuse_file_info *fi)
{
	struct nfsfh *nfsfh = (struct nfsfh *)fi->fh;

	update_rpc_credentials();

	return nfs_pread(nfs, nfsfh, offset, size, buf);
}

static int fuse_nfs_write(const char *path, const char *buf, size_t size,
       off_t offset, struct fuse_file_info *fi)
{
	struct nfsfh *nfsfh = (struct nfsfh *)fi->fh;

        update_rpc_credentials();

	return nfs_pwrite(nfs, nfsfh, offset, size, discard_const(buf));
}

static int fuse_nfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	int ret = 0;
	struct nfsfh *nfsfh;

	update_rpc_credentials();

	ret = nfs_creat(nfs, path, mode, &nfsfh);
	if (ret < 0) {
		return ret;
	}

	fi->fh = (uint64_t)nfsfh;

	return ret;
}

static int fuse_nfs_utime(const char *path, struct utimbuf *times)
{
	update_rpc_credentials();

	return nfs_utime(nfs, path, times);
}

static int fuse_nfs_unlink(const char *path)
{
	update_rpc_credentials();

	return nfs_unlink(nfs, path);
}

static int fuse_nfs_rmdir(const char *path)
{
	update_rpc_credentials();

	return nfs_rmdir(nfs, path);
}

static int fuse_nfs_mkdir(const char *path, mode_t mode)
{
	int ret = 0;

	update_rpc_credentials();

	ret = nfs_mkdir(nfs, path);
	if (ret < 0) {
		return ret;
	}
	ret = nfs_chmod(nfs, path, mode);
	if (ret < 0) {
		return ret;
	}

	return ret;
}

static int fuse_nfs_mknod(const char *path, mode_t mode, dev_t rdev)
{
	update_rpc_credentials();

	return nfs_mknod(nfs, path, mode, rdev);
}

static int fuse_nfs_symlink(const char *from, const char *to)
{
	update_rpc_credentials();

	return nfs_symlink(nfs, from, to);
}

static int fuse_nfs_rename(const char *from, const char *to)
{
	update_rpc_credentials();

	return nfs_rename(nfs, from, to);
}

static int fuse_nfs_link(const char *from, const char *to)
{
	update_rpc_credentials();

	return nfs_link(nfs, from, to);
}

static int fuse_nfs_chmod(const char *path, mode_t mode)
{
	update_rpc_credentials();

	return nfs_chmod(nfs, path, mode);
}

static int fuse_nfs_chown(const char *path, uid_t uid, gid_t gid)
{
	update_rpc_credentials();

	return nfs_chown(nfs, path, uid, gid);
}

static int fuse_nfs_truncate(const char *path, off_t size)
{
	update_rpc_credentials();

	return nfs_truncate(nfs, path, size);
}

static int fuse_nfs_fsync(const char *path, int isdatasync,
			  struct fuse_file_info *fi)
{
	struct nfsfh *nfsfh = (struct nfsfh *)fi->fh;

	update_rpc_credentials();

	return nfs_fsync(nfs, nfsfh);
}

static int fuse_nfs_statfs(const char *path, struct statvfs* stbuf)
{
        int ret = 0;
        struct statvfs svfs;

        ret = nfs_statvfs(nfs, path, &svfs);
  
        stbuf->f_bsize      = svfs.f_bsize;
        stbuf->f_frsize     = svfs.f_frsize;
        stbuf->f_fsid       = svfs.f_fsid;
        stbuf->f_flag       = svfs.f_flag;
        stbuf->f_namemax    = svfs.f_namemax;
        stbuf->f_blocks     = svfs.f_blocks;
        stbuf->f_bfree      = svfs.f_bfree;
        stbuf->f_bavail     = svfs.f_bavail;
        stbuf->f_files      = svfs.f_files;
        stbuf->f_ffree      = svfs.f_ffree;
        stbuf->f_favail     = svfs.f_favail;

        return ret;
}

static struct fuse_operations nfs_oper = {
	.chmod		= fuse_nfs_chmod,
	.chown		= fuse_nfs_chown,
	.create		= fuse_nfs_create,
	.fsync		= fuse_nfs_fsync,
	.getattr	= fuse_nfs_getattr,
	.link		= fuse_nfs_link,
	.mkdir		= fuse_nfs_mkdir,
	.mknod		= fuse_nfs_mknod,
	.open		= fuse_nfs_open,
	.read		= fuse_nfs_read,
	.readdir	= fuse_nfs_readdir,
	.readlink	= fuse_nfs_readlink,
	.release	= fuse_nfs_release,
	.rmdir		= fuse_nfs_rmdir,
	.unlink		= fuse_nfs_unlink,
	.utime		= fuse_nfs_utime,
	.rename		= fuse_nfs_rename,
	.symlink	= fuse_nfs_symlink,
	.truncate	= fuse_nfs_truncate,
	.write		= fuse_nfs_write,
        .statfs 	= fuse_nfs_statfs,
};

void print_usage(char *name)
{
	printf("Usage : %s \n",name);

	printf( "\t [-?|--help] \n"
			"\nfuse-nfs options : \n"
			"\t [-U NFS_UID|--fusenfs_uid=NFS_UID] \n"
			"\t\t The uid passed within the rpc credentials within the mount point \n"
			"\t\t This is the same as passing the uid within the url, however if both are defined then the url's one is used\n"
			"\t [-G NFS_GID|--fusenfs_gid=NFS_GID] \n"
			"\t\t The gid passed within the rpc credentials within the mount point \n"
			"\t\t This is the same as passing the gid within the url, however if both are defined then the url's one is used\n"
			"\t [-o|--fusenfs_allow_other_own_ids] \n"
			"\t\t Allow fuse-nfs with allow_user activated to update the rpc credentials with the current (other) user credentials instead\n"
			"\t\t of using the mount user credentials or (if defined) the custom credentials defined with -U/-G / url \n" 
			"\t\t This option activate allow_other, note that allow_other need user_allow_other to be defined in fuse.conf \n"
			"\nlibnfs options : \n"
			"\t [-n SHARE|--nfs_share=SHARE] \n"
			"\t\t The server export to be mounted \n"
			"\t [-m MNTPOINT|--mountpoint=MNTPOINT] \n"
			"\t\t The client mount point \n"
			"\nfuse options (see man mount.fuse): \n"
			"\t [-p [0|1]|--default_permissions=[0|1]] \n"
			"\t\t The fuse default_permissions option do not have any argument , for compatibility with previous fuse-nfs version default is activated (1)\n"
			"\t\t with the possibility to overwrite this behavior (0) \n"
			"\t [-t [0|1]|--multithread=[0|1]] \n"
			"\t\t Single threaded by default (0) , may have issue with nfs and fuse multithread (1) \n"
			"\t [-a|--allow_other] \n"
			"\t [-r|--allow_root] \n"
			"\t [-u FUSE_UID|--uid=FUSE_UID] \n"
			"\t [-g FUSE_GID|--gid=FUSE_GID] \n"
			"\t [-U UMASK|--umask=UMASK] \n"
			"\t [-d|--direct_io] \n"
			"\t [-k|--kernel_cache] \n"
			"\t [-c|--auto_cache] \n"
			"\t [-E TIMEOUT|--entry_timeout=TIMEOUT] \n"
			"\t [-N TIMEOUT|--negative_timeout=TIMEOUT] \n"
			"\t [-T TIMEOUT|--attr_timeout=TIMEOUT] \n"
			"\t [-C TIMEOUT|--ac_attr_timeout=TIMEOUT] \n"
			"\t [-l|--large_read] \n"
			"\t [-R MAX_READ|--max_read=MAX_READ] \n"
			"\t [-H MAX_READAHEAD|--max_readahead=MAX_READAHEAD] \n"
			"\t [-A|--async_read] \n"
			"\t [-S|--sync_read] \n"
			"\t [-W MAX_WRITE|--max_write=MAX_WRITE] \n"
			"\t\t Default is 32768 \n"
			"\t [-h|--hard_remove] \n"
			"\t [-Y|--nonempty] \n"
			"\t [-q|--use_ino] \n"
			"\t [-Q|--readdir_ino] \n"
			"\t [-f FSNAME|--fsname=FSNAME] \n"
			"\t\t Default is the SHARE provided with -m \n"
			"\t [-s SUBTYPE|--subtype=SUBTYPE] \n"
			"\t\t Default is fuse-nfs with kernel prefexing with fuse. \n"
			"\t [-b|--blkdev] \n"
			"\t [-D|--debug] \n"
			"\t [-i|--intr] \n"
			"\t [-I SIGNAL|--intr_signal=SIGNAL] \n"
			"\t [-O|--read_only] \n"
			);
	exit(0);
}

int main(int argc, char *argv[])
{
	mount_user_uid=getuid();
	mount_user_gid=getgid();

	int ret = 0;
	static struct option long_opts[] = {
		/*fuse-nfs options*/
		{ "help", no_argument, 0, '?' },
		{ "nfs_share", required_argument, 0, 'n' },
		{ "mountpoint", required_argument, 0, 'm' },
		{ "fusenfs_uid", required_argument, 0, 'U' },
		{ "fusenfs_gid", required_argument, 0, 'G' },
		{ "fusenfs_allow_other_own_ids", no_argument, 0, 'o' },
		/*fuse options*/
		{ "allow_other", no_argument, 0, 'a' },
		{ "uid", required_argument, 0, 'u' },
		{ "gid", required_argument, 0, 'g' },
		{ "debug", no_argument, 0, 'D' },
		{ "default_permissions", required_argument, 0, 'p' },
		{ "direct_io", no_argument, 0, 'd' },
		{ "allow_root", no_argument, 0, 'r' },
		{ "kernel_cache", no_argument, 0, 'k' },
		{ "auto_cache", no_argument, 0, 'c' },
		{ "large_read", no_argument, 0, 'l' },
		{ "hard_remove", no_argument, 0, 'h' },
		{ "fsname", required_argument, 0, 'f' },
		{ "subtype", required_argument, 0, 's' },
		{ "blkdev", no_argument, 0, 'b' },
		{ "intr", no_argument, 0, 'i' },
		{ "max_read", required_argument, 0, 'R' },
		{ "max_readahead", required_argument, 0, 'H' },
		{ "async_read", no_argument, 0, 'A' },
		{ "sync_read", no_argument, 0, 'S' },
		{ "umask", required_argument, 0, 'U' },
		{ "entry_timeout", required_argument, 0, 'E' },
		{ "negative_timeout", required_argument, 0, 'N' },
		{ "attr_timeout", required_argument, 0, 'T' },
		{ "ac_attr_timeout", required_argument, 0, 'C' },
		{ "nonempty", no_argument, 0, 'Y' },
		{ "intr_signal", required_argument, 0, 'I' },
		{ "use_ino", no_argument, 0, 'q' },
		{ "readdir_ino", required_argument, 0, 'Q' },
		{ "multithread", required_argument, 0, 't' },
		{ "read_only", no_argument, 0, 'O' },
		{ NULL, 0, 0, 0 }
	};

	int c;
	int opt_idx = 0;
	char *url = NULL;
	char *mnt = NULL;
	char *idstr = NULL;

	char *nfs_uid = NULL;
	char *nfs_gid = NULL;
	char fuse_uid_arg[32];
	char fuse_gid_arg[32];
	char fuse_fsname_arg[1024];
	char fuse_subtype_arg[1024];
	char fuse_max_write_arg[32];
	char fuse_max_read_arg[32];
	char fuse_max_readahead_arg[32];
	char fuse_Umask_arg[32];
	char fuse_entry_timeout_arg[32];
	char fuse_negative_timeout_arg[32];
	char fuse_attr_timeout_arg[32];
	char fuse_ac_attr_timeout_arg[32];
	char fuse_intr_signal_arg[32];

	struct nfs_url *urls = NULL;

	int fuse_nfs_argc = 2;
	char *fuse_nfs_argv[34] = {
		"fuse-nfs",
		"<export>",
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
        };

	while ((c = getopt_long(argc, argv, "?am:n:U:G:u:g:Dp:drklhf:s:biR:W:H:ASK:E:N:T:C:oYI:qQctO", long_opts, &opt_idx)) > 0) {
		switch (c) {
		case '?':
			print_usage(argv[0]);
			return 0;
		case 'a':
			fuse_nfs_argv[fuse_nfs_argc++] = "-oallow_other";
			break;
		case 'm':
			mnt = strdup(optarg);
			break;
		case 'n':
			url = strdup(optarg);
			break;
		case 'U':
			nfs_uid = strdup(optarg);
			custom_uid=atoi(nfs_uid);
			break;
		case 'G':
			nfs_gid = strdup(optarg);
			custom_gid=atoi(nfs_gid);
			break;
		case 'u':
			snprintf(fuse_uid_arg, sizeof(fuse_uid_arg), "-ouid=%s", strdup(optarg));
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_uid_arg;
			break;
		case 'g':
			snprintf(fuse_gid_arg, sizeof(fuse_gid_arg), "-ogid=%s", strdup(optarg));
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_gid_arg;
			break;
		case 'D':
			fuse_nfs_argv[fuse_nfs_argc++] = "-odebug";
			break;
		case 'p':
			fuse_default_permissions=atoi(strdup(optarg));
			break;
		case 't':
			fuse_multithreads=atoi(strdup(optarg));
			break;
		case 'd':
			fuse_nfs_argv[fuse_nfs_argc++] = "-odirect_io";
			break;
		case 'r':
			fuse_nfs_argv[fuse_nfs_argc++] = "-oallow_root";
			break;
		case 'k':
			fuse_nfs_argv[fuse_nfs_argc++] = "-okernel_cache";
			break;
		case 'c':
			fuse_nfs_argv[fuse_nfs_argc++] = "-oauto_cache";
			break;
		case 'l':
			fuse_nfs_argv[fuse_nfs_argc++] = "-olarge_read";
			break;
		case 'h':
			fuse_nfs_argv[fuse_nfs_argc++] = "-ohard_remove";
			break;
		case 'f':
			snprintf(fuse_fsname_arg, sizeof(fuse_fsname_arg), "-ofsname=%s", strdup(optarg));
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_fsname_arg;
			break;
		case 's':
			snprintf(fuse_subtype_arg, sizeof(fuse_subtype_arg), "-osubtype=%s", strdup(optarg));
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_subtype_arg;
			break;
		case 'b':
			fuse_nfs_argv[fuse_nfs_argc++] = "-oblkdev";
			break;
		case 'i':
			fuse_nfs_argv[fuse_nfs_argc++] = "-ointr";
			break;
		case 'R':
			snprintf(fuse_max_read_arg, sizeof(fuse_max_read_arg), "-omax_read=%s", strdup(optarg));
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_max_read_arg;
			break;
		case 'W':
			snprintf(fuse_max_write_arg, sizeof(fuse_max_write_arg), "-omax_write=%s", strdup(optarg));
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_max_write_arg;
			break;
		case 'H':
			snprintf(fuse_max_readahead_arg, sizeof(fuse_max_readahead_arg), "-omax_readahead=%s", strdup(optarg));
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_max_readahead_arg;
			break;
		case 'A':
			fuse_nfs_argv[fuse_nfs_argc++] = "-oasync_read";
			break;
		case 'S':
			fuse_nfs_argv[fuse_nfs_argc++] = "-osync_read";
			break;
		case 'K':
			snprintf(fuse_Umask_arg, sizeof(fuse_Umask_arg), "-oumask=%s", strdup(optarg));
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_Umask_arg;
			break;
		case 'E':
			snprintf(fuse_entry_timeout_arg, sizeof(fuse_entry_timeout_arg), "-oentry_timeout=%s", strdup(optarg));
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_entry_timeout_arg;
			break;
		case 'N':
			snprintf(fuse_negative_timeout_arg, sizeof(fuse_negative_timeout_arg), "-onegative_timeout=%s", strdup(optarg));
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_negative_timeout_arg;
			break;
		case 'T':
			snprintf(fuse_attr_timeout_arg, sizeof(fuse_attr_timeout_arg), "-oattr_timeout=%s", strdup(optarg));
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_attr_timeout_arg;
			break;
		case 'C':
			snprintf(fuse_ac_attr_timeout_arg, sizeof(fuse_ac_attr_timeout_arg), "-oac_attr_timeout=%s", strdup(optarg));
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_ac_attr_timeout_arg;
			break;
		case 'o':
            fusenfs_allow_other_own_ids=1;
			break;
		case 'Y':
			fuse_nfs_argv[fuse_nfs_argc++] = "-ononempty";
			break;
		case 'I':
			snprintf(fuse_intr_signal_arg, sizeof(fuse_intr_signal_arg), "-ointr_signal=%s", strdup(optarg));
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_intr_signal_arg;
			break;
		case 'q':
			fuse_nfs_argv[fuse_nfs_argc++] = "-ouse_ino";
			break;
		case 'Q':
			fuse_nfs_argv[fuse_nfs_argc++] = "-oreaddir_ino";
			break;
	        case 'O':
			fuse_nfs_argv[fuse_nfs_argc++] = "-oro";
			break;
		}
	}

	if (url == NULL) {
		fprintf(stderr, "-n was not specified.\n");
		print_usage(argv[0]);
		ret = 10;
		goto finished;
	}
	if (mnt == NULL) {
		fprintf(stderr, "-m was not specified.\n");
		print_usage(argv[0]);
		ret = 10;
		goto finished;
	}
	
	/* Set allow_other if not defined and fusenfs_allow_other_own_ids defined */
	if (fusenfs_allow_other_own_ids)
	{
		int i,allow_other_set=0;
		for(i ; i < fuse_nfs_argc; ++i)
		{
			if(!strcmp(fuse_nfs_argv[i], "-oallow_other"))
			{
				allow_other_set=1;
				break;
			}
		}
		if (!allow_other_set){fuse_nfs_argv[fuse_nfs_argc++] = "-oallow_other";}
	}

	/* Set default fsname if not defined */
	if (!strstr(fuse_fsname_arg, "-ofsname="))
	{
		snprintf(fuse_fsname_arg, sizeof(fuse_fsname_arg), "-ofsname=%s", url);
		fuse_nfs_argv[fuse_nfs_argc++] = fuse_fsname_arg;
	}

	/* Set default subtype if not defined */
	if (!strstr(fuse_subtype_arg, "-osubtype="))
	{
		snprintf(fuse_subtype_arg, sizeof(fuse_subtype_arg), "-osubtype=%s", "fuse-nfs");
		fuse_nfs_argv[fuse_nfs_argc++] = fuse_subtype_arg;
	}

	if (!strstr(fuse_max_write_arg, "-omax_write="))
	{
		snprintf(fuse_max_write_arg, sizeof(fuse_max_write_arg), "-omax_write=%s", "32768");
		fuse_nfs_argv[fuse_nfs_argc++] = fuse_max_write_arg;
	}

	/* Only for compatibility with previous version */
	if (fuse_default_permissions){fuse_nfs_argv[fuse_nfs_argc++] = "-odefault_permissions";}
	if (!fuse_multithreads){fuse_nfs_argv[fuse_nfs_argc++] = "-s";}

	nfs = nfs_init_context();
	if (nfs == NULL) {
		fprintf(stderr, "Failed to init context\n");
		ret = 10;
		goto finished;
	}

	urls = nfs_parse_url_dir(nfs, url);
	if (urls == NULL) {
		fprintf(stderr, "Failed to parse url : %s\n", nfs_get_error(nfs));
		ret = 10;
		goto finished;
	}

	if (idstr = strstr(url, "uid=")) { custom_uid = atoi(&idstr[4]); }
	if (idstr = strstr(url, "gid=")) { custom_gid = atoi(&idstr[4]); }

	#ifdef WIN32
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2,2),&wsaData);
	#endif

	ret = nfs_mount(nfs, urls->server, urls->path);
	if (ret != 0) {
		fprintf(stderr, "Failed to mount nfs share : %s\n", nfs_get_error(nfs));
		goto finished;
	}

	fuse_nfs_argv[1] = mnt;
	return fuse_main(fuse_nfs_argc, fuse_nfs_argv, &nfs_oper, NULL);

finished:
	nfs_destroy_url(urls);
	if (nfs != NULL) {
		nfs_destroy_context(nfs);
	}
	free(url);
	free(mnt);
	return ret;
}
