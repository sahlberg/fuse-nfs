/* CLOSE submission failures must return without waiting for a missing callback. */
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
static int test_close(struct nfs_context *, struct nfsfh *, nfs_cb, void *);

#define CHECK(x) do { if (!(x)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        exit(EXIT_FAILURE); \
} } while (0)

#define main fuse_nfs_program_main
#define nfs_get_fd test_get_fd
#define nfs_which_events test_which_events
#define nfs_service test_service
#define nfs_close_async test_close
#include "fuse-nfs.c"
#undef main
#undef nfs_get_fd
#undef nfs_which_events
#undef nfs_service
#undef nfs_close_async

static int submission_status, completion_status, complete_inline;
static int submissions, callbacks, released_handles;
static struct nfsfh *pending_handle;
static nfs_cb pending_callback;
static void *pending_opaque;

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

static int test_close(struct nfs_context *context, struct nfsfh *handle,
                      nfs_cb cb, void *opaque)
{
        CHECK(pthread_mutex_trylock(&nfs_mutex) == EBUSY);
        CHECK(pending_callback == NULL);
        submissions++;
        if (submission_status < 0 || complete_inline) {
                /* Model libnfs owning the handle, including on failure. */
                free(handle);
                released_handles++;
                if (submission_status == 0) {
                        callbacks++;
                        cb(completion_status, context, NULL, opaque);
                }
                return submission_status;
        }
        pending_handle = handle;
        pending_callback = cb;
        pending_opaque = opaque;
        return 0;
}

static int test_service(struct nfs_context *context, int revents)
{
        CHECK(revents == 0);
        CHECK(pthread_mutex_trylock(&nfs_mutex) == EBUSY);
        if (pending_callback) {
                nfs_cb cb = pending_callback;
                void *opaque = pending_opaque;

                pending_callback = NULL;
                pending_opaque = NULL;
                free(pending_handle);
                pending_handle = NULL;
                released_handles++;
                callbacks++;
                cb(completion_status, context, NULL, opaque);
        }
        return 0;
}

static void release_result(int submit_status, int reply_status, int inline_reply)
{
        struct fuse_file_info fi = {0};
        void *handle = malloc(1);
        int before = callbacks;

        CHECK(handle != NULL);
        fi.fh = (uint64_t)handle;
        submission_status = submit_status;
        completion_status = reply_status;
        complete_inline = inline_reply;
        CHECK(fuse_nfs_release("/file", &fi) ==
              (submit_status < 0 ? submit_status : reply_status));
        CHECK(callbacks == before + (submit_status == 0));
        CHECK(submissions == released_handles);
        CHECK(pending_callback == NULL && pending_handle == NULL);
}

int main(void)
{
        const int failures[] = {-1, -ENOMEM, -EIO};
        unsigned int i;

        alarm(5); /* The old release implementation hangs on the first failure. */
        nfs = nfs_init_context();
        CHECK(nfs != NULL);
        nfs_oper.init(NULL);
        for (i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
                release_result(failures[i], 0, 0);
                release_result(0, 0, 0); /* Service still works after rejection. */
        }
        release_result(0, -EIO, 0);
        release_result(0, 0, 1);
        release_result(0, -EIO, 1);
        stop_nfs_service();
        nfs_destroy_context(nfs);
        puts("PASS: CLOSE rejects unqueued work, propagates completion errors, and services later requests");
        return 0;
}
#endif
