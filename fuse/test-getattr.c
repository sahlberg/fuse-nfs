/* Failed getattr must leave its output untouched and allow later requests. */
#ifdef WIN32
int main(void) { return 77; }
#else
#define FUSE_USE_VERSION 26
#define _FILE_OFFSET_BITS 64
#include <assert.h>
#include <signal.h>
#include <nfsc/libnfs.h>

static int test_get_fd(struct nfs_context *);
static int test_which_events(struct nfs_context *);
static int test_service(struct nfs_context *, int);
static int test_lstat(struct nfs_context *, const char *, nfs_cb, void *);

#define main fuse_nfs_program_main
#define nfs_get_fd test_get_fd
#define nfs_which_events test_which_events
#define nfs_service test_service
#define nfs_lstat64_async test_lstat
#include "fuse-nfs.c"
#undef main
#undef nfs_get_fd
#undef nfs_which_events
#undef nfs_service
#undef nfs_lstat64_async

static nfs_cb pending_callback;
static void *pending_opaque;
static int pending_status;

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
        assert(pthread_mutex_trylock(&nfs_mutex) == EBUSY);
        assert(pending_callback == NULL);
        if (strcmp(path, "/queue-failure") == 0)
                return -ENOMEM;
        pending_callback = cb;
        pending_opaque = opaque;
        pending_status = strcmp(path, "/missing/.nomedia") == 0 ? -ENOENT :
                         strcmp(path, "/denied") == 0 ? -EACCES : 0;
        return 0;
}

static int test_service(struct nfs_context *context, int revents)
{
        assert(revents == 0);
        assert(pthread_mutex_trylock(&nfs_mutex) == EBUSY);
        if (pending_callback) {
                struct nfs_stat_64 st = {0};
                st.nfs_mode = S_IFREG | 0444;
                st.nfs_size = 1234;
                pending_callback(pending_status, context,
                                 pending_status ? NULL : &st, pending_opaque);
                pending_callback = NULL;
        }
        return 0;
}

int main(void)
{
        struct FUSE_STAT st, untouched;
        const char *paths[] = { "/missing/.nomedia", "/denied", "/queue-failure" };
        int statuses[] = { -ENOENT, -EACCES, -ENOMEM };
        alarm(5);
        nfs = nfs_init_context();
        assert(nfs != NULL);
        custom_uid = custom_gid = 0;
        nfs_oper.init(NULL);
        for (int i = 0; i < 3; i++) {
                memset(&st, 0xa5, sizeof(st));
                memcpy(&untouched, &st, sizeof(st));
                assert(fuse_nfs_getattr(paths[i], &st) == statuses[i]);
                assert(memcmp(&st, &untouched, sizeof(st)) == 0);
                assert(fuse_nfs_getattr("/existing", &st) == 0);
                assert(st.st_size == 1234 && S_ISREG(st.st_mode));
        }
        stop_nfs_service();
        nfs_destroy_context(nfs);
        puts("PASS: getattr errors preserve output and subsequent requests succeed");
        return 0;
}
#endif
