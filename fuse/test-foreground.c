/* Exercise argument forwarding without mounting NFS or FUSE. */
#ifdef WIN32
int main(void) { return 77; }
#else
#define FUSE_USE_VERSION 26
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <nfsc/libnfs.h>
#include <fuse.h>

static int test_mount(struct nfs_context *, const char *, const char *);
static int test_fuse_main(int, char **, const struct fuse_operations *, size_t, void *);

#define CHECK(x) do { if (!(x)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        exit(EXIT_FAILURE); \
} } while (0)

#define main fuse_nfs_program_main
#define nfs_mount test_mount
#undef fuse_main
#define fuse_main(argc, argv, op, data) \
        test_fuse_main(argc, argv, op, sizeof(*(op)), data)
#include "fuse-nfs.c"
#undef main
#undef nfs_mount
#undef fuse_main

static int expected_foreground, expected_debug, fuse_calls;

static int test_mount(struct nfs_context *context, const char *server, const char *path)
{
        (void)context;
        CHECK(strcmp(server, "127.0.0.1") == 0);
        CHECK(strcmp(path, "/export") == 0);
        return 0;
}

static int test_fuse_main(int argc, char **argv, const struct fuse_operations *op,
                          size_t size, void *data)
{
        int foreground_count = 0, debug_count = 0;
        (void)op; (void)size; (void)data;
        CHECK(argv[argc] == NULL);
        for (int i = 0; i < argc; i++) {
                foreground_count += strcmp(argv[i], "-f") == 0;
                debug_count += strcmp(argv[i], "-odebug") == 0;
        }
        CHECK(foreground_count == expected_foreground);
        CHECK(debug_count == expected_debug);
        fuse_calls++;
        return 0;
}

static void run_case(char **options, int foreground, int debug)
{
        pid_t child;
        int status;
        fflush(NULL);
        child = fork();
        CHECK(child >= 0);
        if (child == 0) {
                char *argv[96] = { "fuse-nfs", "-n", "nfs://127.0.0.1/export",
                                   "-m", "/unused-mount" };
                int argc = 5;
                alarm(5);
                for (int i = 0; options[i]; i++)
                        argv[argc++] = options[i];
                expected_foreground = foreground;
                expected_debug = debug;
                CHECK(fuse_nfs_program_main(argc, argv) == 0);
                CHECK(fuse_calls == 1);
                _exit(0);
        }
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void)
{
        char *defaults[] = { NULL };
        char *short_option[] = { "-F", NULL };
        char *long_option[] = { "--foreground", NULL };
        char *debug[] = { "-D", NULL };
        char *repeated[] = { "-FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF", NULL };
        /* Maximum distinct forwarded options plus the automatic options. */
        char *many[] = { "-F", "-a", "-u", "0", "-g", "0", "-D", "-d",
                "-r", "-k", "-c", "-l", "-h", "-f", "test", "-s", "test",
                "-b", "-i", "-R", "32768", "-W", "32768", "-H", "32768",
                "-A", "-S", "-K", "022", "-E", "1", "-N", "1", "-T", "1",
                "-C", "1", "-Y", "-I", "10", "-q", "-Q", "-O", "-t", "0",
                NULL };
        run_case(defaults, 0, 0);
        run_case(short_option, 1, 0);
        run_case(long_option, 1, 0);
        run_case(debug, 0, 1);
        run_case(repeated, 1, 0);
        run_case(many, 1, 1);
        puts("PASS: foreground forwarding, default/debug behavior, and argument capacity");
        return 0;
}
#endif
