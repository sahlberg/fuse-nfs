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

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <nfsc/libnfs.h>

#include "../config.h"

#define discard_const(ptr) ((void *)((intptr_t)(ptr)))

struct nfs_context *nfs = NULL;
int custom_uid = -1;
int custom_gid = -1;

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

static int fuse_nfs_getattr(const char *path, struct stat *stbuf)
{
	int ret = 0;
	struct nfs_stat_64 nfs_st;

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
#ifdef HAVE_ST_ATIM
	stbuf->st_atim.tv_sec  = nfs_st.nfs_atime;
	stbuf->st_atim.tv_nsec = nfs_st.nfs_atime_nsec;
	stbuf->st_mtim.tv_sec  = nfs_st.nfs_mtime;
	stbuf->st_mtim.tv_nsec = nfs_st.nfs_mtime_nsec;
	stbuf->st_ctim.tv_sec  = nfs_st.nfs_ctime;
	stbuf->st_ctim.tv_nsec = nfs_st.nfs_ctime_nsec;
#else
	stbuf->st_atime      = nfs_st.nfs_atime;
	stbuf->st_atime_nsec = nfs_st.nfs_atime_nsec;
	stbuf->st_mtime      = nfs_st.nfs_mtime;
	stbuf->st_mtime_nsec = nfs_st.nfs_mtime_nsec;
	stbuf->st_ctime      = nfs_st.nfs_ctime;
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
	return nfs_readlink(nfs, path, buf, size);
}

static int fuse_nfs_open(const char *path, struct fuse_file_info *fi)
{
	int ret = 0;
	struct nfsfh *nfsfh;

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
	int ret = 0;
	struct nfsfh *nfsfh = (struct nfsfh *)fi->fh;

	ret = nfs_pread(nfs, nfsfh, offset, size, buf);

	return ret;
}

static int fuse_nfs_write(const char *path, const char *buf, size_t size,
       off_t offset, struct fuse_file_info *fi)
{
	int ret = 0;
	struct nfsfh *nfsfh = (struct nfsfh *)fi->fh;

	ret = nfs_pwrite(nfs, nfsfh, offset, size, discard_const(buf));

	return ret;
}

static int fuse_nfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	int ret = 0;
	struct nfsfh *nfsfh;

	ret = nfs_creat(nfs, path, mode, &nfsfh);
	if (ret < 0) {
		return ret;
	}

	fi->fh = (uint64_t)nfsfh;

	return ret;
}

static int fuse_nfs_utime(const char *path, struct utimbuf *times)
{
	int ret = 0;

	ret = nfs_utime(nfs, path, times);
	if (ret < 0) {
		return ret;
	}

	return ret;
}

static int fuse_nfs_unlink(const char *path)
{
	int ret = 0;

	ret = nfs_unlink(nfs, path);
	if (ret < 0) {
		return ret;
	}

	return ret;
}

static int fuse_nfs_rmdir(const char *path)
{
	int ret = 0;

	ret = nfs_rmdir(nfs, path);
	if (ret < 0) {
		return ret;
	}

	return ret;
}

static int fuse_nfs_mkdir(const char *path, mode_t mode)
{
	int ret = 0;

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
	return nfs_mknod(nfs, path, mode, rdev);
}

static int fuse_nfs_symlink(const char *from, const char *to)
{
	return nfs_symlink(nfs, from, to);
}

static int fuse_nfs_rename(const char *from, const char *to)
{
	return nfs_rename(nfs, from, to);
}

static int fuse_nfs_link(const char *from, const char *to)
{
	return nfs_link(nfs, from, to);
}

static int fuse_nfs_chmod(const char *path, mode_t mode)
{
	return nfs_chmod(nfs, path, mode);
}

static int fuse_nfs_chown(const char *path, uid_t uid, gid_t gid)
{
	return nfs_chown(nfs, path, uid, gid);
}

static int fuse_nfs_truncate(const char *path, off_t size)
{
	return nfs_truncate(nfs, path, size);
}

static int fuse_nfs_fsync(const char *path, int isdatasync,
			  struct fuse_file_info *fi)
{
	struct nfsfh *nfsfh = (struct nfsfh *)fi->fh;

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
	printf("Usage: %s [-?|--help] [-a|--allow-other] [-n|--nfs-share=nfs-url] [-m|--mountpoint=mountpoint]\n",
		name);
	exit(0);
}

int main(int argc, char *argv[])
{
	int ret = 0;
	static struct option long_opts[] = {
		{ "help", no_argument, 0, '?' },
		{ "allow-other", no_argument, 0, 'a' },
		{ "nfs-share", required_argument, 0, 'n' },
		{ "mountpoint", required_argument, 0, 'm' },
		{ NULL, 0, 0, 0 }
	};
        char buffer[1024];
	int c;
	int opt_idx = 0;
	char *url = NULL;
	char *mnt = NULL;
    char *idstr = NULL;
  	struct nfs_url *urls = NULL;
	int fuse_nfs_argc = 5;
	char *fuse_nfs_argv[16] = {
		"fuse-nfs",
		"<export>",
		"-odefault_permissions",
		"-omax_write=32768",
		"-s",
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

	while ((c = getopt_long(argc, argv, "?ham:n:", long_opts,
		    &opt_idx)) > 0) {
		switch (c) {
		case 'h':
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
        // add FS type to 'url'
        snprintf(buffer, sizeof(buffer), "-ofsname=%s", url);
        fuse_nfs_argv[fuse_nfs_argc++] = buffer;
  
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

	if (idstr = strstr(url, "uid=")) {
        custom_uid = atoi(&idstr[4]);
    }
    if (idstr = strstr(url, "gid=")) {
        custom_gid = atoi(&idstr[4]);
    }

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
