/* Opt-in public API regression against a read-only NFSv4 export. */
#ifdef WIN32
int main(void) { return 77; }
#else
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw.h>

#define CHECK(x) do { if (!(x)) { \
        fprintf(stderr, "FAIL: line %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)

struct result {
        int calls, status;
        struct nfsfh *fh;
};

static void complete(int status, struct nfs_context *nfs, void *data, void *opaque)
{
        struct result *r = opaque;
        (void)nfs;
        (void)data;
        CHECK(++r->calls == 1);
        r->status = status;
}

static void opened(int status, struct nfs_context *nfs, void *data, void *opaque)
{
        struct result *r = opaque;
        complete(status, nfs, data, opaque);
        if (status == 0)
                r->fh = data;
}

static void await_reply(struct nfs_context *nfs, struct result *r)
{
        while (!r->calls) {
                struct pollfd pfd = { .fd = nfs_get_fd(nfs),
                                      .events = nfs_which_events(nfs) };
                int ret = poll(&pfd, 1, 100);
                if (ret < 0 && errno == EINTR)
                        continue;
                CHECK(ret >= 0);
                ret = nfs_service(nfs, pfd.revents);
                if (ret < 0)
                        fprintf(stderr, "RPC service failed: %s\n",
                                rpc_get_error(nfs_get_rpc_context(nfs)));
                CHECK(ret == 0);
        }
}

int main(void)
{
        const char *url_text = getenv("FUSE_NFS_TEST_URL");
        const char *existing = getenv("FUSE_NFS_TEST_FILE");
        struct nfs_context *nfs;
        struct nfs_url *url;
        struct nfsfh *fh;
        struct result r = {0};
        char missing[128];
#ifdef LIBNFS_API_V2
        char buffer[4096];
#endif
        if (!url_text || !existing) {
                puts("SKIP: set FUSE_NFS_TEST_URL (export URL) and FUSE_NFS_TEST_FILE (nonempty file within export)");
                return 77;
        }
        alarm(60);
        nfs = nfs_init_context();
        CHECK(nfs != NULL);
        CHECK(nfs_set_version(nfs, 4) == 0);
        url = nfs_parse_url_dir(nfs, url_text);
        CHECK(url != NULL);
        CHECK(nfs_get_version(nfs) == 4);
        nfs_set_readonly(nfs, 1);
        CHECK(nfs_mount_async(nfs, url->server, url->path, complete, &r) == 0);
        await_reply(nfs, &r);
        CHECK(r.status == 0);
        nfs_destroy_url(url);

        snprintf(missing, sizeof(missing), "/.fuse-nfs-enoent-test-%ld/.nomedia", (long)getpid());
        memset(&r, 0, sizeof(r));
        CHECK(nfs_lstat64_async(nfs, missing, complete, &r) == 0);
        await_reply(nfs, &r);
        CHECK(r.status == -ENOENT);
        puts("PASS: missing path returned ENOENT with successful RPC servicing");

        memset(&r, 0, sizeof(r));
        CHECK(nfs_lstat64_async(nfs, existing, complete, &r) == 0);
        await_reply(nfs, &r);
        CHECK(r.status == 0);
        memset(&r, 0, sizeof(r));
        CHECK(nfs_open_async(nfs, existing, O_RDONLY, opened, &r) == 0);
        await_reply(nfs, &r);
        CHECK(r.status == 0 && r.fh != NULL);
        fh = r.fh;
        memset(&r, 0, sizeof(r));
#ifdef LIBNFS_API_V2
        CHECK(nfs_pread_async(nfs, fh, buffer, sizeof(buffer), 0, complete, &r) == 0);
#else
        CHECK(nfs_pread_async(nfs, fh, 0, 4096, complete, &r) == 0);
#endif
        await_reply(nfs, &r);
        CHECK(r.status > 0);
        memset(&r, 0, sizeof(r));
        CHECK(nfs_close_async(nfs, fh, complete, &r) == 0);
        await_reply(nfs, &r);
        CHECK(r.status == 0);
        nfs_destroy_context(nfs);
        puts("PASS: subsequent stat/open/read/close succeeded on the same context");
        return 0;
}
#endif
