/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
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
#ifndef WIN32
#include <poll.h>
#endif
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <nfsc/libnfs.h>

#ifdef WIN32
#include <winsock2.h>
#include <win32/win32_compat.h>
#endif

#ifndef FUSE_STAT
#define FUSE_STAT stat
#endif

#define LOG(...) do {                                              \
        if (logfile) {                                          \
                FILE *fh = fopen(logfile, "a+");                \
                time_t t = time(NULL);                          \
                char tmp[256];                                  \
                strftime(tmp, sizeof(tmp), "%T", localtime(&t));\
                fprintf(fh, "[NFS] %s ", tmp);			\
                fprintf(fh, __VA_ARGS__);                       \
                fclose(fh);                                     \
        }                                                       \
} while (0);

static char *logfile;

/* Only one thread at a time can enter libnfs */
static pthread_mutex_t nfs_mutex = PTHREAD_MUTEX_INITIALIZER;

#define discard_const(ptr) ((void *)((intptr_t)(ptr)))

struct nfs_context *nfs = NULL;
int custom_uid = -1;
int custom_gid = -1;

uid_t mount_user_uid;
gid_t mount_user_gid;

int fusenfs_allow_other_own_ids=0;
int fuse_default_permissions=1;
int fuse_multithreads=1;

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
        return fuse_get_context()->uid;
    }
    return possible_uid;
}

static int map_gid(int possible_gid) {
    if (custom_gid != -1 && possible_gid == custom_gid){
        return fuse_get_context()->gid;
    }
    return possible_gid;
}

static int map_reverse_uid(int possible_uid) {
    if (custom_uid != -1 && possible_uid == getuid()) {
        return custom_uid;
    }
    return possible_uid;
}

static int map_reverse_gid(int possible_gid) {
    if (custom_gid != -1 && possible_gid == getgid()){
        return custom_gid;
    }
    return possible_gid;
}

struct sync_cb_data {
	int is_finished;
	int status;

	void *return_data;
	size_t max_size;
};

static void
wait_for_nfs_reply(struct nfs_context *nfs, struct sync_cb_data *cb_data)
{
	struct pollfd pfd;
	int revents;
	int ret;
	static pthread_mutex_t reply_mutex = PTHREAD_MUTEX_INITIALIZER;

	pthread_mutex_lock(&reply_mutex);
	while (!cb_data->is_finished) {

		pfd.fd = nfs_get_fd(nfs);
		pfd.events = nfs_which_events(nfs);
		pfd.revents = 0;

		ret = poll(&pfd, 1, 100);
		if (ret < 0) {
			revents = -1;
		} else {
			revents = pfd.revents;
		}

		pthread_mutex_lock(&nfs_mutex);
		ret = nfs_service(nfs, revents);
		pthread_mutex_unlock(&nfs_mutex);
		if (ret < 0) {
			cb_data->status = -EIO;
			break;
		}
	}
	pthread_mutex_unlock(&reply_mutex);
}

static void
generic_cb(int status, struct nfs_context *nfs, void *data, void *private_data)
{
	struct sync_cb_data *cb_data = private_data;

	cb_data->is_finished = 1;
	cb_data->status = status;
}

/* Update the rpc credentials to the current user unless
 * have are overriding the credentials via url arguments.
 */
static void update_rpc_credentials(void) {
	if (custom_uid == -1  && !fusenfs_allow_other_own_ids) {
		nfs_set_uid(nfs, fuse_get_context()->uid);
	} else if ((custom_uid == -1 ||
                    fuse_get_context()->uid != mount_user_uid)
                   && fusenfs_allow_other_own_ids) {
		nfs_set_uid(nfs, fuse_get_context()->uid);
	} else {
		nfs_set_uid(nfs, custom_uid);
	}
	if (custom_gid == -1 && !fusenfs_allow_other_own_ids) {
		nfs_set_gid(nfs, fuse_get_context()->gid);
        } else if ((custom_gid == -1 ||
                    fuse_get_context()->gid != mount_user_gid)
                   && fusenfs_allow_other_own_ids) {
		nfs_set_gid(nfs, fuse_get_context()->gid);
	} else {
		nfs_set_gid(nfs, custom_gid);
	}
}

static void
stat64_cb(int status, struct nfs_context *nfs, void *data, void *private_data)
{
	struct sync_cb_data *cb_data = private_data;

	cb_data->is_finished = 1;
	cb_data->status = status;

	LOG("stat64_cb status:%d\n", status);

	if (status < 0) {
		return;
	}
	memcpy(cb_data->return_data, data, sizeof(struct nfs_stat_64));
}

static int
fuse_nfs_getattr(const char *path, struct FUSE_STAT *stbuf)
{
	struct nfs_stat_64 st;
	struct sync_cb_data cb_data;
	int ret;

	LOG("fuse_nfs_getattr entered [%s]\n", path);

        memset(&cb_data, 0, sizeof(struct sync_cb_data));
	cb_data.return_data = &st;

	pthread_mutex_lock(&nfs_mutex);
	update_rpc_credentials();
	ret = nfs_lstat64_async(nfs, path, stat64_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	if (ret < 0) {
		return ret;
	}
	wait_for_nfs_reply(nfs, &cb_data);

	stbuf->st_dev          = st.nfs_dev;
	stbuf->st_ino          = st.nfs_ino;
	stbuf->st_mode         = st.nfs_mode;
	stbuf->st_nlink        = st.nfs_nlink;
	stbuf->st_uid          = map_uid(st.nfs_uid);
	stbuf->st_gid          = map_gid(st.nfs_gid);
	stbuf->st_rdev         = st.nfs_rdev;
	stbuf->st_size         = st.nfs_size;
	stbuf->st_blksize      = st.nfs_blksize;
	stbuf->st_blocks       = st.nfs_blocks;

#if defined(HAVE_ST_ATIM) || defined(__MINGW32__)
	stbuf->st_atim.tv_sec  = st.nfs_atime;
	stbuf->st_atim.tv_nsec = st.nfs_atime_nsec;
	stbuf->st_mtim.tv_sec  = st.nfs_mtime;
	stbuf->st_mtim.tv_nsec = st.nfs_mtime_nsec;
	stbuf->st_ctim.tv_sec  = st.nfs_ctime;
	stbuf->st_ctim.tv_nsec = st.nfs_ctime_nsec;
#else
	stbuf->st_atime      = st.nfs_atime;
	stbuf->st_mtime      = st.nfs_mtime;
	stbuf->st_ctime      = st.nfs_ctime;
	stbuf->st_atime_nsec = st.nfs_atime_nsec;
	stbuf->st_mtime_nsec = st.nfs_mtime_nsec;
	stbuf->st_ctime_nsec = st.nfs_ctime_nsec;
#endif
	return cb_data.status;
}

static void
readdir_cb(int status, struct nfs_context *nfs, void *data, void *private_data)
{
	struct sync_cb_data *cb_data = private_data;

	cb_data->is_finished = 1;
	cb_data->status = status;

	LOG("readdir_cb status:%d\n", status);

	if (status < 0) {
		return;
	}
	cb_data->return_data = data;
}

static int
fuse_nfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		 off_t offset, struct fuse_file_info *fi)
{
	struct nfsdir *nfsdir;
	struct nfsdirent *nfsdirent;
	struct sync_cb_data cb_data;
	int ret;

	LOG("fuse_nfs_readdir entered [%s]\n", path);

        memset(&cb_data, 0, sizeof(struct sync_cb_data));

	pthread_mutex_lock(&nfs_mutex);
        update_rpc_credentials();
	ret = nfs_opendir_async(nfs, path, readdir_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	if (ret < 0) {
		return ret;
	}
	wait_for_nfs_reply(nfs, &cb_data);

	nfsdir = cb_data.return_data;
	while ((nfsdirent = nfs_readdir(nfs, nfsdir)) != NULL) {
		filler(buf, nfsdirent->name, NULL, 0);
	}

	nfs_closedir(nfs, nfsdir);

	return cb_data.status;
}

static void
readlink_cb(int status, struct nfs_context *nfs, void *data, void *private_data)
{
	struct sync_cb_data *cb_data = private_data;

	cb_data->is_finished = 1;
	cb_data->status = status;

	if (status < 0) {
		return;
	}
	strncat(cb_data->return_data, data, cb_data->max_size);
}

static int
fuse_nfs_readlink(const char *path, char *buf, size_t size)
{
	struct sync_cb_data cb_data;
	int ret;

	LOG("fuse_nfs_readlink entered [%s]\n", path);

        memset(&cb_data, 0, sizeof(struct sync_cb_data));
	*buf = 0;
	cb_data.return_data = buf;
	cb_data.max_size = size;

	pthread_mutex_lock(&nfs_mutex);
        update_rpc_credentials();
	ret = nfs_readlink_async(nfs, path, readlink_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	if (ret < 0) {
		return ret;
	}
	wait_for_nfs_reply(nfs, &cb_data);

	return cb_data.status;
}

static void
open_cb(int status, struct nfs_context *nfs, void *data, void *private_data)
{
	struct sync_cb_data *cb_data = private_data;

	cb_data->is_finished = 1;
	cb_data->status = status;

	LOG("open_cb status:%d\n", status);

	if (status < 0) {
		return;
	}
	cb_data->return_data = data;
}

static int
fuse_nfs_open(const char *path, struct fuse_file_info *fi)
{
	struct sync_cb_data cb_data;
	int ret;

	LOG("fuse_nfs_open entered [%s]\n", path);

        memset(&cb_data, 0, sizeof(struct sync_cb_data));

	pthread_mutex_lock(&nfs_mutex);
        update_rpc_credentials();

	fi->fh = 0;
        ret = nfs_open_async(nfs, path, fi->flags, open_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	if (ret < 0) {
		return ret;
	}
	wait_for_nfs_reply(nfs, &cb_data);

	fi->fh = (uint64_t)cb_data.return_data;

	return cb_data.status;
}

static int fuse_nfs_release(const char *path, struct fuse_file_info *fi)
{
	struct sync_cb_data cb_data;
	struct nfsfh *nfsfh = (struct nfsfh *)fi->fh;

        memset(&cb_data, 0, sizeof(struct sync_cb_data));

	pthread_mutex_lock(&nfs_mutex);
	nfs_close_async(nfs, nfsfh, generic_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	wait_for_nfs_reply(nfs, &cb_data);

	return 0;
}

static void
read_cb(int status, struct nfs_context *nfs, void *data, void *private_data)
{
	struct sync_cb_data *cb_data = private_data;

	cb_data->is_finished = 1;
	cb_data->status = status;

	if (status < 0) {
		return;
	}
	memcpy(cb_data->return_data, data, status);
}

static int
fuse_nfs_read(const char *path, char *buf, size_t size,
	      off_t offset, struct fuse_file_info *fi)
{
	struct nfsfh *nfsfh = (struct nfsfh *)fi->fh;
	struct sync_cb_data cb_data;
	int ret;

	LOG("fuse_nfs_read entered [%s]\n", path);

        memset(&cb_data, 0, sizeof(struct sync_cb_data));
	cb_data.return_data = buf;

	pthread_mutex_lock(&nfs_mutex);
	update_rpc_credentials();
	ret = nfs_pread_async(nfs, nfsfh, offset, size, read_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	if (ret < 0) {
		return ret;
	}
	wait_for_nfs_reply(nfs, &cb_data);

	return cb_data.status;
}

static int fuse_nfs_write(const char *path, const char *buf, size_t size,
       off_t offset, struct fuse_file_info *fi)
{
	struct nfsfh *nfsfh = (struct nfsfh *)fi->fh;
	struct sync_cb_data cb_data;
	int ret;

	LOG("fuse_nfs_write entered [%s]\n", path);

        memset(&cb_data, 0, sizeof(struct sync_cb_data));

	pthread_mutex_lock(&nfs_mutex);
        update_rpc_credentials();
	ret = nfs_pwrite_async(nfs, nfsfh, offset, size, discard_const(buf),
			       generic_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	if (ret < 0) {
		return ret;
	}
	wait_for_nfs_reply(nfs, &cb_data);

	return cb_data.status;
}

static int fuse_nfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	struct sync_cb_data cb_data;
	int ret = 0;

	LOG("fuse_nfs_create entered [%s]\n", path);

        memset(&cb_data, 0, sizeof(struct sync_cb_data));

	pthread_mutex_lock(&nfs_mutex);
	update_rpc_credentials();
	ret = nfs_creat_async(nfs, path, mode, open_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	if (ret < 0) {
		return ret;
	}
	wait_for_nfs_reply(nfs, &cb_data);

	fi->fh = (uint64_t)cb_data.return_data;
	
	return cb_data.status;
}

static int fuse_nfs_utime(const char *path, struct utimbuf *times)
{
	struct sync_cb_data cb_data;
	int ret;

	LOG("fuse_nfs_utime entered [%s]\n", path);

        memset(&cb_data, 0, sizeof(struct sync_cb_data));

	pthread_mutex_lock(&nfs_mutex);
	update_rpc_credentials();
	ret = nfs_utime_async(nfs, path, times, generic_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	if (ret < 0) {
                LOG("fuse_nfs_utime returned %d. %s\n", ret,
                    nfs_get_error(nfs));
		return ret;
	}
	wait_for_nfs_reply(nfs, &cb_data);

	return cb_data.status;
}

static int fuse_nfs_unlink(const char *path)
{
	struct sync_cb_data cb_data;
	int ret;

	LOG("fuse_nfs_unlink entered [%s]\n", path);

        memset(&cb_data, 0, sizeof(struct sync_cb_data));

	pthread_mutex_lock(&nfs_mutex);
	update_rpc_credentials();
        ret = nfs_unlink_async(nfs, path, generic_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	if (ret < 0) {
		return ret;
	}
	wait_for_nfs_reply(nfs, &cb_data);

	return cb_data.status;
}

static int fuse_nfs_rmdir(const char *path)
{
	struct sync_cb_data cb_data;
	int ret;

	LOG("fuse_nfs_mknod entered [%s]\n", path);

        memset(&cb_data, 0, sizeof(struct sync_cb_data));

	pthread_mutex_lock(&nfs_mutex);
	update_rpc_credentials();
	ret = nfs_rmdir_async(nfs, path, generic_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	if (ret < 0) {
		return ret;
	}
	wait_for_nfs_reply(nfs, &cb_data);

	return cb_data.status;
}

static int
fuse_nfs_mkdir(const char *path, mode_t mode)
{
	struct sync_cb_data cb_data;
	int ret;

	LOG("fuse_nfs_mkdir entered [%s]\n", path);

        memset(&cb_data, 0, sizeof(struct sync_cb_data));

	pthread_mutex_lock(&nfs_mutex);
	update_rpc_credentials();
	ret = nfs_mkdir_async(nfs, path, generic_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	if (ret < 0) {
		return ret;
	}
	wait_for_nfs_reply(nfs, &cb_data);

	cb_data.is_finished = 0;

	pthread_mutex_lock(&nfs_mutex);
	update_rpc_credentials();
	ret = nfs_chmod_async(nfs, path, mode, generic_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	if (ret < 0) {
		return ret;
	}
	wait_for_nfs_reply(nfs, &cb_data);

	return cb_data.status;
}

static int fuse_nfs_mknod(const char *path, mode_t mode, dev_t rdev)
{
	struct sync_cb_data cb_data;
	int ret;

	LOG("fuse_nfs_mknod entered [%s]\n", path);

        memset(&cb_data, 0, sizeof(struct sync_cb_data));

	pthread_mutex_lock(&nfs_mutex);
	update_rpc_credentials();
	ret = nfs_mknod_async(nfs, path, mode, rdev, generic_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	if (ret < 0) {
		return ret;
	}
	wait_for_nfs_reply(nfs, &cb_data);

	return cb_data.status;
}

static int fuse_nfs_symlink(const char *from, const char *to)
{
	struct sync_cb_data cb_data;
	int ret;

	LOG("fuse_nfs_symlink entered [%s -> %s]\n", from, to);

        memset(&cb_data, 0, sizeof(struct sync_cb_data));

	pthread_mutex_lock(&nfs_mutex);
	update_rpc_credentials();
	ret = nfs_symlink_async(nfs, from, to, generic_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	if (ret < 0) {
		return ret;
	}
	wait_for_nfs_reply(nfs, &cb_data);

	return cb_data.status;
}

static int fuse_nfs_rename(const char *from, const char *to)
{
	struct sync_cb_data cb_data;
	int ret;

	LOG("fuse_nfs_rename entered [%s -> %s]\n", from, to);

        memset(&cb_data, 0, sizeof(struct sync_cb_data));

	pthread_mutex_lock(&nfs_mutex);
	update_rpc_credentials();
	ret = nfs_rename_async(nfs, from, to, generic_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	if (ret < 0) {
		return ret;
	}
	wait_for_nfs_reply(nfs, &cb_data);

	return cb_data.status;
}

static int
fuse_nfs_link(const char *from, const char *to)
{
	struct sync_cb_data cb_data;
	int ret;

	LOG("fuse_nfs_link entered [%s -> %s]\n", from, to);

        memset(&cb_data, 0, sizeof(struct sync_cb_data));

	pthread_mutex_lock(&nfs_mutex);
	update_rpc_credentials();
	ret = nfs_link_async(nfs, from, to, generic_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	if (ret < 0) {
		return ret;
	}
	wait_for_nfs_reply(nfs, &cb_data);
	
	return cb_data.status;
}

static int
fuse_nfs_chmod(const char *path, mode_t mode)
{
	struct sync_cb_data cb_data;
	int ret;

	LOG("fuse_nfs_chmod entered [%s]\n", path);

        memset(&cb_data, 0, sizeof(struct sync_cb_data));

	pthread_mutex_lock(&nfs_mutex);
	update_rpc_credentials();
	ret = nfs_chmod_async(nfs, path, mode, generic_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	if (ret < 0) {
		return ret;
	}
	wait_for_nfs_reply(nfs, &cb_data);
	
	return cb_data.status;
}

static int fuse_nfs_chown(const char *path, uid_t uid, gid_t gid)
{
	struct sync_cb_data cb_data;
	int ret;

	LOG("fuse_nfs_chown entered [%s]\n", path);

        memset(&cb_data, 0, sizeof(struct sync_cb_data));

	pthread_mutex_lock(&nfs_mutex);
	update_rpc_credentials();
	ret = nfs_chown_async(nfs, path,
			      map_reverse_uid(uid), map_reverse_gid(gid),
			      generic_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	if (ret < 0) {
		return ret;
	}
	wait_for_nfs_reply(nfs, &cb_data);
	
	return cb_data.status;
}

static int fuse_nfs_truncate(const char *path, off_t size)
{
	struct sync_cb_data cb_data;
	int ret;

	LOG("fuse_nfs_truncate entered [%s]\n", path);

        memset(&cb_data, 0, sizeof(struct sync_cb_data));

	pthread_mutex_lock(&nfs_mutex);
	update_rpc_credentials();
	ret = nfs_truncate_async(nfs, path, size, generic_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	if (ret < 0) {
		return ret;
	}
	wait_for_nfs_reply(nfs, &cb_data);

	return cb_data.status;
}

static int
fuse_nfs_fsync(const char *path, int isdatasync,
	       struct fuse_file_info *fi)
{
	struct nfsfh *nfsfh = (struct nfsfh *)fi->fh;
	struct sync_cb_data cb_data;
	int ret;

	LOG("fuse_nfs_fsync entered [%s]\n", path);

        memset(&cb_data, 0, sizeof(struct sync_cb_data));

	pthread_mutex_lock(&nfs_mutex);
	update_rpc_credentials();
        ret = nfs_fsync_async(nfs, nfsfh, generic_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	if (ret < 0) {
		return ret;
	}
	wait_for_nfs_reply(nfs, &cb_data);
	
	return cb_data.status;
}

static void
statvfs_cb(int status, struct nfs_context *nfs, void *data, void *private_data)
{
	struct sync_cb_data *cb_data = private_data;

	cb_data->is_finished = 1;
	cb_data->status = status;

	if (status < 0) {
		return;
	}
	memcpy(cb_data->return_data, data, sizeof(struct statvfs));
}

static int
fuse_nfs_statfs(const char *path, struct statvfs* stbuf)
{
        int ret;
        struct statvfs svfs;

	struct sync_cb_data cb_data;

	LOG("fuse_nfs_statfs entered [%s]\n", path);

        memset(&cb_data, 0, sizeof(struct sync_cb_data));
	cb_data.return_data = &svfs;

	pthread_mutex_lock(&nfs_mutex);
        ret = nfs_statvfs_async(nfs, path, statvfs_cb, &cb_data);
	pthread_mutex_unlock(&nfs_mutex);
	if (ret < 0) {
		return ret;
	}
	wait_for_nfs_reply(nfs, &cb_data);
  
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

	return cb_data.status;
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
			"\t\t Multi-threaded by default (1) \n"
			"\t [-a|--allow_other] \n"
			"\t [-r|--allow_root] \n"
			"\t [-u FUSE_UID|--uid=FUSE_UID] \n"
			"\t [-g FUSE_GID|--gid=FUSE_GID] \n"
			"\t [-K UMASK|--umask=UMASK] \n"
			"\t [-d|--direct_io] \n"
			"\t [-k|--kernel_cache] \n"
			"\t [-c|--auto_cache] \n"
			"\t [-E TIMEOUT|--entry_timeout=TIMEOUT] \n"
			"\t [-N TIMEOUT|--negative_timeout=TIMEOUT] \n"
			"\t [-T TIMEOUT|--attr_timeout=TIMEOUT] \n"
			"\t [-C TIMEOUT|--ac_attr_timeout=TIMEOUT] \n"
			"\t [-L|--logfile=logfile] \n"
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
                { "logfile", required_argument, 0, 'L' },
		{ "hard_remove", no_argument, 0, 'h' },
		{ "fsname", required_argument, 0, 'f' },
		{ "subtype", required_argument, 0, 's' },
		{ "blkdev", no_argument, 0, 'b' },
		{ "intr", no_argument, 0, 'i' },
		{ "max_read", required_argument, 0, 'R' },
		{ "max_readahead", required_argument, 0, 'H' },
		{ "async_read", no_argument, 0, 'A' },
		{ "sync_read", no_argument, 0, 'S' },
		{ "umask", required_argument, 0, 'K' },
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

	char fuse_uid_arg[32] = {0};
	char fuse_gid_arg[32] = {0};
	char fuse_fsname_arg[1024] = {0};
	char fuse_subtype_arg[1024] = {0};
	char fuse_max_write_arg[32] = {0};
	char fuse_max_read_arg[32] = {0};
	char fuse_max_readahead_arg[32] = {0};
	char fuse_Umask_arg[32] = {0};
	char fuse_entry_timeout_arg[32] = {0};
	char fuse_negative_timeout_arg[32] = {0};
	char fuse_attr_timeout_arg[32] = {0};
	char fuse_ac_attr_timeout_arg[32] = {0};
	char fuse_intr_signal_arg[32] = {0};

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

	while ((c = getopt_long(argc, argv, "?am:n:U:G:u:g:Dp:drklL:hf:s:biR:W:H:ASK:E:N:T:C:oYI:qQct:O", long_opts, &opt_idx)) > 0) {
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
			custom_uid=atoi(optarg);
			break;
		case 'G':
			custom_gid=atoi(optarg);
			break;
		case 'u':
			snprintf(fuse_uid_arg, sizeof(fuse_uid_arg), "-ouid=%s", optarg);
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_uid_arg;
			break;
		case 'g':
			snprintf(fuse_gid_arg, sizeof(fuse_gid_arg), "-ogid=%s", optarg);
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_gid_arg;
			break;
		case 'D':
			fuse_nfs_argv[fuse_nfs_argc++] = "-odebug";
			break;
		case 'p':
			fuse_default_permissions=atoi(optarg);
			break;
		case 't':
			fuse_multithreads=atoi(optarg);
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
                case 'L':
                        logfile = strdup(optarg);
                        break;
		case 'h':
			fuse_nfs_argv[fuse_nfs_argc++] = "-ohard_remove";
			break;
		case 'f':
			snprintf(fuse_fsname_arg, sizeof(fuse_fsname_arg), "-ofsname=%s", optarg);
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_fsname_arg;
			break;
		case 's':
			snprintf(fuse_subtype_arg, sizeof(fuse_subtype_arg), "-osubtype=%s", optarg);
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_subtype_arg;
			break;
		case 'b':
			fuse_nfs_argv[fuse_nfs_argc++] = "-oblkdev";
			break;
		case 'i':
			fuse_nfs_argv[fuse_nfs_argc++] = "-ointr";
			break;
		case 'R':
			snprintf(fuse_max_read_arg, sizeof(fuse_max_read_arg), "-omax_read=%s", optarg);
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_max_read_arg;
			break;
		case 'W':
			snprintf(fuse_max_write_arg, sizeof(fuse_max_write_arg), "-omax_write=%s", optarg);
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_max_write_arg;
			break;
		case 'H':
			snprintf(fuse_max_readahead_arg, sizeof(fuse_max_readahead_arg), "-omax_readahead=%s", optarg);
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_max_readahead_arg;
			break;
		case 'A':
			fuse_nfs_argv[fuse_nfs_argc++] = "-oasync_read";
			break;
		case 'S':
			fuse_nfs_argv[fuse_nfs_argc++] = "-osync_read";
			break;
		case 'K':
			snprintf(fuse_Umask_arg, sizeof(fuse_Umask_arg), "-oumask=%s", optarg);
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_Umask_arg;
			break;
		case 'E':
			snprintf(fuse_entry_timeout_arg, sizeof(fuse_entry_timeout_arg), "-oentry_timeout=%s", optarg);
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_entry_timeout_arg;
			break;
		case 'N':
			snprintf(fuse_negative_timeout_arg, sizeof(fuse_negative_timeout_arg), "-onegative_timeout=%s", optarg);
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_negative_timeout_arg;
			break;
		case 'T':
			snprintf(fuse_attr_timeout_arg, sizeof(fuse_attr_timeout_arg), "-oattr_timeout=%s", optarg);
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_attr_timeout_arg;
			break;
		case 'C':
			snprintf(fuse_ac_attr_timeout_arg, sizeof(fuse_ac_attr_timeout_arg), "-oac_attr_timeout=%s", optarg);
			fuse_nfs_argv[fuse_nfs_argc++] = fuse_ac_attr_timeout_arg;
			break;
		case 'o':
            fusenfs_allow_other_own_ids=1;
			break;
		case 'Y':
			fuse_nfs_argv[fuse_nfs_argc++] = "-ononempty";
			break;
		case 'I':
			snprintf(fuse_intr_signal_arg, sizeof(fuse_intr_signal_arg), "-ointr_signal=%s", optarg);
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
		int i = 0, allow_other_set=0;
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

	LOG("Starting fuse_main()\n");
	ret = fuse_main(fuse_nfs_argc, fuse_nfs_argv, &nfs_oper, NULL);

finished:
	nfs_destroy_url(urls);
	if (nfs != NULL) {
		nfs_destroy_context(nfs);
	}
	free(url);
	free(mnt);
	return ret;
}
