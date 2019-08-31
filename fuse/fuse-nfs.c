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
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
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

uid_t mount_user_uid;
gid_t mount_user_gid;

struct fuse_nfs_args {
	int help;
	int version;
	int ignore;
	int allow_other_own_ids;
	int no_default_permissions;
	long custom_uid;
	long custom_gid;
	const char* nfs_share;
	const char* mountpoint;
	int multithreaded;
};

struct fuse_nfs_args args;

#define FUSE_NFS_OPT(t, p, v) { t, offsetof(struct fuse_nfs_args, p), v }
#define FUSE_NFS_OPT_LS(s, t, p, v) FUSE_NFS_OPT("-" s, p, v), FUSE_NFS_OPT(t, p, v)
static const struct fuse_opt fuse_nfs_options[] = {
	FUSE_NFS_OPT_LS("U", "fusenfs_uid", custom_uid, 1),
	FUSE_NFS_OPT_LS("G", "fusenfs_gid", custom_gid, 1),
	FUSE_NFS_OPT_LS("A", "fusenfs_allow_other_own_ids", allow_other_own_ids, 0),
	FUSE_NFS_OPT_LS("t", "multithread", multithreaded, 1),
	FUSE_NFS_OPT("no_default_permissions", no_default_permissions, 1),
	FUSE_NFS_OPT("users", ignore, 1),
	FUSE_NFS_OPT("-?", help, 1),
	FUSE_NFS_OPT_LS("h", "help", help, 1),
	FUSE_OPT_END
};
#undef FUSE_NFS_OPT


#ifdef __MINGW32__
gid_t getgid(){
	if( args.custom_gid == -1 )
		return 65534;
	return args.custom_gid;
}

uid_t getuid(){
	if( args.custom_uid == -1 )
		return 65534;
	return args.custom_uid;
}
#endif

static int map_uid(int possible_uid) {
    if (args.custom_uid != -1 && possible_uid == args.custom_uid){
        return fuse_get_context()->uid;
    }
    return possible_uid;
}

static int map_gid(int possible_gid) {
    if (args.custom_gid != -1 && possible_gid == args.custom_gid){
        return fuse_get_context()->gid;
    }
    return possible_gid;
}

static int map_reverse_uid(int possible_uid) {
    if (args.custom_uid != -1 && possible_uid == getuid()) {
        return args.custom_uid;
    }
    return possible_uid;
}

static int map_reverse_gid(int possible_gid) {
    if (args.custom_gid != -1 && possible_gid == getgid()){
        return args.custom_gid;
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
 * have are overriding the credentials via url args.
 */
static void update_rpc_credentials(void) {
	if (args.custom_uid == -1  && !args.allow_other_own_ids) {
		nfs_set_uid(nfs, fuse_get_context()->uid);
	} else if ((args.custom_uid == -1 ||
                    fuse_get_context()->uid != mount_user_uid)
                   && args.allow_other_own_ids) {
		nfs_set_uid(nfs, fuse_get_context()->uid);
	} else {
		nfs_set_uid(nfs, args.custom_uid);
	}
	if (args.custom_gid == -1 && !args.allow_other_own_ids) {
		nfs_set_gid(nfs, fuse_get_context()->gid);
        } else if ((args.custom_gid == -1 ||
                    fuse_get_context()->gid != mount_user_gid)
                   && args.allow_other_own_ids) {
		nfs_set_gid(nfs, fuse_get_context()->gid);
	} else {
		nfs_set_gid(nfs, args.custom_gid);
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

void print_help(char* name){
	printf("usage: %s [options] share mountpoint\n\n", name);

	printf( "\t [-?|-h|--help] \n"
			"\nfuse-nfs options: \n"
			"\n"
			"\t share\n"
			"\t\t The server export to be mounted\n"
			"\t mountpoint\n"
			"\t\t The directory to mount the nfs share on\n"
			"\n"
			"\t [-U NFS_UID|-o fusenfs_uid=NFS_UID] \n"
			"\t\t The uid passed within the rpc credentials within the mount point \n"
			"\t\t This is the same as passing the uid within the url, however if both are defined then the url's one is used\n"
			"\t [-G NFS_GID|-o fusenfs_gid=NFS_GID] \n"
			"\t\t The gid passed within the rpc credentials within the mount point \n"
			"\t\t This is the same as passing the gid within the url, however if both are defined then the url's one is used\n"
			"\t [-A|-o fusenfs_allow_other_own_ids] \n"
			"\t\t Allow fuse-nfs with allow_user activated to update the rpc credentials with the current (other) user credentials instead\n"
			"\t\t of using the mount user credentials or (if defined) the custom credentials defined with -U/-G / url \n" 
			"\t\t This option activate allow_other, note that allow_other need user_allow_other to be defined in fuse.conf \n"
			"\t [-t|-o multithread]\n"
			"\t\t Enables multithreading. (it's disabled per default using the -s option). Enabling fuse multithreading may cause issues\n"
			"\t [-o no_default_permissions]\n"
			"\t\t The fuse option default_permissions is on per default, this option turns it off.\n"
	);

	printf("\nfuse ");
	fflush(stdout);
	fuse_main(2, ((char*[]){"program", "--help",0}), NULL, NULL);
}

static int fuse_nfs_optparse_proc(void *data, const char *arg, int key, struct fuse_args *outargs){
  (void)key;
  (void)outargs;
  struct fuse_nfs_args* args = data;
  if (key == FUSE_OPT_KEY_NONOPT && !args->nfs_share)
  {
    args->nfs_share = arg;
    return 0;
  } else if (!args->mountpoint)
  {
    args->mountpoint = arg;
    return 1;
  }
  return 1;
}

int main(int argc, char *argv[])
{
	int ret = 0;
	struct nfs_url* urls = 0;

	mount_user_uid = getuid();
	mount_user_gid = getgid();

	#ifdef WIN32
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2,2),&wsaData);
	#endif

	nfs = nfs_init_context();
	if (nfs == NULL) {
		fprintf(stderr, "Failed to init context\n");
		ret = 10;
		goto finished;
	}

	args.custom_uid = -1;
	args.custom_gid = -1;

	struct fuse_args fuse_args = FUSE_ARGS_INIT(argc, argv);
	int result = fuse_opt_parse(&fuse_args, &args, fuse_nfs_options, fuse_nfs_optparse_proc);

	if (args.help){
		print_help(argv[0]);
		goto finished;
	}

	if (!args.nfs_share || !args.mountpoint || result != 0) {
		print_help(argv[0]);
		ret = 1;
		goto finished;
	}

	if (!args.multithreaded) {
		if (fuse_opt_add_arg(&fuse_args, "-s") == -1) {
			perror("fuse_opt_add_arg failed");
			ret = 2;
			goto finished;
		}
	}

	if (!args.no_default_permissions) {
		if (fuse_opt_add_arg(&fuse_args, "-odefault_permissions") == -1) {
			perror("fuse_opt_add_arg failed");
			ret = 2;
			goto finished;
		}
	}

	urls = nfs_parse_url_dir(nfs, args.nfs_share);
	if (urls == NULL) {
		fprintf(stderr, "Failed to parse url : %s\n", nfs_get_error(nfs));
		ret = 10;
		goto finished;
	}

	ret = nfs_mount(nfs, urls->server, urls->path);
	if (ret != 0) {
		fprintf(stderr, "Failed to mount nfs share : %s\n", nfs_get_error(nfs));
		ret = 20;
		goto finished;
	}

	LOG("Starting fuse_main()\n");
	ret = fuse_main(fuse_args.argc, fuse_args.argv, &nfs_oper, NULL);

finished:
	if(urls)
		nfs_destroy_url(urls);
	if (nfs != NULL) {
		nfs_destroy_context(nfs);
	}
	return ret;
}
