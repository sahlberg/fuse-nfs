/* Failed metadata requests must not consume results or prevent later requests. */
#ifdef WIN32
int main(void) { return 77; }
#else
#define FUSE_USE_VERSION 26
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <nfsc/libnfs.h>

static int test_get_fd(struct nfs_context *);
static int test_which_events(struct nfs_context *);
static int test_service(struct nfs_context *, int);
static int test_lstat(struct nfs_context *, const char *, nfs_cb, void *);
static int test_statvfs(struct nfs_context *, const char *, nfs_cb, void *);
static int test_opendir(struct nfs_context *, const char *, nfs_cb, void *);
static struct nfsdirent *test_readdir(struct nfs_context *, struct nfsdir *);
static void test_closedir(struct nfs_context *, struct nfsdir *);

#define CHECK(x) do { if (!(x)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        exit(EXIT_FAILURE); \
} } while (0)

#define main fuse_nfs_program_main
#define nfs_get_fd test_get_fd
#define nfs_which_events test_which_events
#define nfs_service test_service
#define nfs_lstat64_async test_lstat
#define nfs_statvfs_async test_statvfs
#define nfs_opendir_async test_opendir
#define nfs_readdir test_readdir
#define nfs_closedir test_closedir
#include "fuse-nfs.c"
#undef main
#undef nfs_get_fd
#undef nfs_which_events
#undef nfs_service
#undef nfs_lstat64_async
#undef nfs_statvfs_async
#undef nfs_opendir_async
#undef nfs_readdir
#undef nfs_closedir

static nfs_cb pending_callback;
static void *pending_opaque;
static int pending_status;
static enum { ATTR, STATFS, READDIR } pending_kind;
static char directory_token;
static int directory_reads, directory_closes;

static int test_get_fd(struct nfs_context *context)
{
        (void)context;
        return -1;
}

static int test_which_events(struct nfs_context *context)
{
        (void)context;
        return 0;
}

static int test_lstat(struct nfs_context *context, const char *path,
                      nfs_cb cb, void *opaque)
{
        (void)context;
        pending_kind = ATTR;
        CHECK(pthread_mutex_trylock(&nfs_mutex) == EBUSY);
        CHECK(pending_callback == NULL);
        if (strcmp(path, "/queue-failure") == 0)
                return -ENOMEM;
        pending_callback = cb;
        pending_opaque = opaque;
        pending_status = strcmp(path, "/missing/.nomedia") == 0 ? -ENOENT :
                         strcmp(path, "/denied") == 0 ? -EACCES : 0;
        return 0;
}

static int test_statvfs(struct nfs_context *context, const char *path,
                        nfs_cb cb, void *opaque)
{
        int ret = test_lstat(context, path, cb, opaque);
        pending_kind = STATFS;
        return ret;
}

static int test_opendir(struct nfs_context *context, const char *path,
                        nfs_cb cb, void *opaque)
{
        int ret = test_lstat(context, path, cb, opaque);
        pending_kind = READDIR;
        return ret;
}

static struct nfsdirent *test_readdir(struct nfs_context *context,
                                    struct nfsdir *dir)
{
        (void)context;
        CHECK(dir == (struct nfsdir *)&directory_token);
        directory_reads++;
        return NULL; /* A successfully opened empty directory. */
}

static void test_closedir(struct nfs_context *context, struct nfsdir *dir)
{
        (void)context;
        CHECK(dir == (struct nfsdir *)&directory_token);
        CHECK(pthread_mutex_trylock(&nfs_mutex) == EBUSY);
        directory_closes++;
}

static int test_service(struct nfs_context *context, int revents)
{
        CHECK(revents == 0);
        CHECK(pthread_mutex_trylock(&nfs_mutex) == EBUSY);
        if (pending_callback) {
                struct nfs_stat_64 st = {0};
                struct statvfs svfs = {0};
                void *result;
                st.nfs_mode = S_IFREG | 0444;
                st.nfs_size = 1234;
                svfs.f_bsize = 4096;
                svfs.f_blocks = 1234;
                result = pending_kind == ATTR ? (void *)&st :
                         pending_kind == STATFS ? (void *)&svfs :
                         (void *)&directory_token;
                pending_callback(pending_status, context,
                                 pending_status ? NULL : result, pending_opaque);
                pending_callback = NULL;
        }
        return 0;
}

int main(void)
{
        struct FUSE_STAT st, untouched;
        struct statvfs svfs, svfs_untouched;
        const char *paths[] = { "/missing/.nomedia", "/denied", "/queue-failure" };
        int statuses[] = { -ENOENT, -EACCES, -ENOMEM };
        alarm(5);
        nfs = nfs_init_context();
        CHECK(nfs != NULL);
        custom_uid = custom_gid = 0;
        nfs_oper.init(NULL);
        for (int i = 0; i < 3; i++) {
                memset(&st, 0xa5, sizeof(st));
                memcpy(&untouched, &st, sizeof(st));
                CHECK(fuse_nfs_getattr(paths[i], &st) == statuses[i]);
                CHECK(memcmp(&st, &untouched, sizeof(st)) == 0);
                CHECK(fuse_nfs_getattr("/existing", &st) == 0);
                CHECK(st.st_size == 1234 && S_ISREG(st.st_mode));

                memset(&svfs, 0xa5, sizeof(svfs));
                memcpy(&svfs_untouched, &svfs, sizeof(svfs));
                CHECK(fuse_nfs_statfs(paths[i], &svfs) == statuses[i]);
                CHECK(memcmp(&svfs, &svfs_untouched, sizeof(svfs)) == 0);
                CHECK(fuse_nfs_statfs("/existing", &svfs) == 0);
                CHECK(svfs.f_bsize == 4096 && svfs.f_blocks == 1234);

                directory_reads = directory_closes = 0;
                CHECK(fuse_nfs_readdir(paths[i], NULL, NULL, 0, NULL) == statuses[i]);
                CHECK(directory_reads == 0 && directory_closes == 0);
                CHECK(fuse_nfs_readdir("/existing", NULL, NULL, 0, NULL) == 0);
                CHECK(directory_reads == 1 && directory_closes == 1);
        }
        stop_nfs_service();
        nfs_destroy_context(nfs);
        puts("PASS: metadata errors preserve output and subsequent requests succeed");
        return 0;
}
#endif
