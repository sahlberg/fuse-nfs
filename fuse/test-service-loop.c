/* This test uses assertions with side effects; keep them active in all builds. */
#undef NDEBUG
/* Exercise the real FUSE event loop without a server or a FUSE mount. */
#ifdef WIN32
int main(void) { return 77; } /* POSIX signals and fork are used by this test. */
#else
#define FUSE_USE_VERSION 26
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include <time.h>
#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw.h>

static struct nfs_context *test_init_context(void);
static int test_mount(struct nfs_context *, const char *, const char *);
static FILE *test_fopen(const char *, const char *);
static int test_get_fd(struct nfs_context *context);
static int test_which_events(struct nfs_context *context);
static int test_service(struct nfs_context *context, int revents);
static const char *test_get_error(struct nfs_context *context);
static const char *test_rpc_error(struct rpc_context *rpc);
static int test_poll(struct pollfd *fds, nfds_t count, int timeout);

#define CHECK(x) do { if (!(x)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        exit(EXIT_FAILURE); \
} } while (0)

#define main fuse_nfs_program_main
#define nfs_get_fd test_get_fd
#define nfs_which_events test_which_events
#define nfs_service test_service
#define nfs_get_error test_get_error
#define rpc_get_error test_rpc_error
#define poll test_poll
#define fopen test_fopen
#define nfs_init_context test_init_context
#define nfs_mount test_mount
#include "fuse-nfs.c"
#undef fopen
#undef nfs_init_context
#undef nfs_mount
#undef main
#undef nfs_get_fd
#undef nfs_which_events
#undef nfs_service
#undef nfs_get_error
#undef poll
#undef rpc_get_error

#define WORKERS 8
#define ROUNDS 200
static int server_pipe[2];
static int service_calls, completions, fail_service, fail_poll;
static int fail_init, fail_mount, startup_log_calls;
static const char *expected_startup_error;
static pthread_t service_owner;
static int have_owner;
static struct sync_cb_data *pending[WORKERS];
static int interrupt_next, interrupts;
static const char *captured_stderr_path;

static struct nfs_context *test_init_context(void)
{
        return fail_init ? NULL : nfs_init_context();
}

static int test_mount(struct nfs_context *context, const char *server, const char *path)
{
        return fail_mount ? -EIO : nfs_mount(context, server, path);
}

static FILE *test_fopen(const char *path, const char *mode)
{
        if (expected_startup_error && path == logfile) {
                char line[2048];
                FILE *capture = fopen(captured_stderr_path, "r");
                CHECK(capture != NULL);
                CHECK(fgets(line, sizeof(line), capture) != NULL);
                CHECK(strstr(line, expected_startup_error) != NULL);
                fclose(capture);
                startup_log_calls++;
                errno = EACCES;
                return NULL;
        }
        if (fail_service && captured_stderr_path && path == logfile) {
                char line[2048];
                FILE *capture = fopen(captured_stderr_path, "r");
                CHECK(capture != NULL);
                /* The fallback must already be visible before logfile I/O. */
                CHECK(fgets(line, sizeof(line), capture) != NULL);
                CHECK(strstr(line, "rpc_error=[injected connection failure]") != NULL);
                fclose(capture);
        }
        return fopen(path, mode);
}

static const char *test_rpc_error(struct rpc_context *rpc)
{
        (void)rpc;
        return "injected connection failure";
}

static int test_poll(struct pollfd *fds, nfds_t count, int timeout)
{
        int ret;
        pthread_mutex_lock(&nfs_mutex);
        if (interrupt_next) {
                interrupt_next = 0;
                interrupts++;
                pthread_mutex_unlock(&nfs_mutex);
                fds[0].revents = POLLERR;
                errno = EINTR;
                return -1;
        }
        pthread_mutex_unlock(&nfs_mutex);
        ret = poll(fds, count, timeout);
        pthread_mutex_lock(&nfs_mutex);
        if (fail_service) {
                fds[0].revents = POLLIN | POLLERR | POLLHUP;
                ret = 1;
                if (fail_poll) {
                        errno = EBADF;
                        ret = -1;
                }
        }
        pthread_mutex_unlock(&nfs_mutex);
        return ret;
}

static int test_get_fd(struct nfs_context *context)
{
        (void)context;
        return server_pipe[0];
}

static int test_which_events(struct nfs_context *context)
{
        (void)context;
        return POLLIN;
}

static const char *test_get_error(struct nfs_context *context)
{
        (void)context;
        return "NFS4ERR_NOENT: stale pathname diagnostic";
}

static int test_service(struct nfs_context *context, int revents)
{
        (void)context;
        /* EINTR must not be reported as a socket error. */
        CHECK(revents == 0 || (fail_service &&
               (revents == (POLLIN | POLLERR | POLLHUP) ||
                (fail_poll && revents == -1))));
        CHECK(pthread_mutex_trylock(&nfs_mutex) == EBUSY);
        if (!have_owner) {
                service_owner = pthread_self();
                have_owner = 1;
        }
        CHECK(pthread_equal(service_owner, pthread_self()));
        service_calls++;
        if (fail_service) {
                errno = EIO; /* Must not replace the captured poll errno. */
                return -1;
        }
        for (int i = 0; i < WORKERS; i++) {
                if (pending[i]) {
                        /* Publish the flag first, just like the real callbacks. */
                        pending[i]->is_finished = 1;
                        pending[i]->status = i % 2 ? -ENOENT : 1000 + i;
                        *(int *)pending[i]->return_data = 2000 + i;
                        pending[i] = NULL;
                        completions++;
                }
        }
        return 0;
}
static void start_loop(void)
{
        if (!nfs_oper.init) {
                fprintf(stderr, "FAIL: no background NFS service lifecycle\n");
                exit(1);
        }
        CHECK(pipe(server_pipe) == 0);
        service_calls = completions = fail_service = have_owner = 0;
        memset(pending, 0, sizeof(pending));
        nfs_oper.init(NULL);
}

static void stop_loop(void)
{
#ifndef TEST_BASELINE
        stop_nfs_service();
        stop_nfs_service();
#endif
        close(server_pipe[0]);
        close(server_pipe[1]);
}

static void *request_worker(void *arg)
{
        int id = *(int *)arg;
        for (int i = 0; i < ROUNDS; i++) {
                int result = -1;
                struct sync_cb_data cb = {0};
                cb.return_data = &result;
                pthread_mutex_lock(&nfs_mutex);
                CHECK(pending[id] == NULL);
                pending[id] = &cb;
                pthread_mutex_unlock(&nfs_mutex);
                wait_for_nfs_reply(nfs, &cb);
                CHECK(cb.status == (id % 2 ? -ENOENT : 1000 + id));
                CHECK(result == 2000 + id);
        }
        return NULL;
}

static void check_startup_failure(int failure, const char *expected)
{
        char capture_path[] = "./fuse-nfs-startup-XXXXXX";
        int fd = mkstemp(capture_path), status;
        pid_t child;
        CHECK(fd >= 0);
        close(fd);
        fflush(NULL);
        child = fork();
        CHECK(child >= 0);
        if (child == 0) {
                char *argv[] = { "fuse-nfs", "-n",
                        failure == 1 ? "invalid-url" : "nfs://127.0.0.1/export",
                        "-m", "/unused", "-L", "/unused-log", NULL };
                alarm(3);
                captured_stderr_path = capture_path;
                expected_startup_error = expected;
                fail_init = failure == 0;
                fail_mount = failure == 2;
                CHECK(freopen(capture_path, "w", stderr) != NULL);
                /* Force buffering: the error must be flushed before fopen. */
                CHECK(setvbuf(stderr, NULL, _IOFBF, 4096) == 0);
                CHECK(fuse_nfs_program_main(7, argv) == (failure == 2 ? -EIO : 10));
                CHECK(startup_log_calls > 0);
                _exit(0);
        }
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        unlink(capture_path);
}

int main(void)
{
        pthread_t workers[WORKERS];
        int ids[WORKERS];
        int before, after, status;
        pid_t child;
        struct timespec pause = {0, 400000000};

        nfs = nfs_init_context();
        CHECK(nfs != NULL);
        custom_uid = custom_gid = 0;
        alarm(20);
        start_loop();
        nanosleep(&pause, NULL);
        pthread_mutex_lock(&nfs_mutex);
        CHECK(service_calls >= 2);
        before = service_calls;
        pthread_mutex_unlock(&nfs_mutex);
        puts("PASS: NFS service runs while FUSE is idle");

        pthread_mutex_lock(&nfs_mutex);
        interrupt_next = 1;
        pthread_mutex_unlock(&nfs_mutex);
        nanosleep(&pause, NULL);
        pthread_mutex_lock(&nfs_mutex);
        CHECK(service_calls > before);
        CHECK(interrupts == 1);
        pthread_mutex_unlock(&nfs_mutex);
        puts("PASS: interrupted poll keeps the service loop alive");

        for (int i = 0; i < WORKERS; i++) {
                ids[i] = i;
                CHECK(pthread_create(&workers[i], NULL, request_worker, &ids[i]) == 0);
        }
        for (int i = 0; i < WORKERS; i++)
                CHECK(pthread_join(workers[i], NULL) == 0);
        CHECK(completions == WORKERS * ROUNDS);
        puts("PASS: 1600 concurrent replies, one service owner, complete results");

        {
                struct sync_cb_data cb = { .is_finished = 1, .status = 42 };
                wait_for_nfs_reply(nfs, &cb);
                CHECK(cb.status == 42);
        }
        puts("PASS: reply completed before wait is not lost");
        stop_loop();
        before = service_calls;
        nanosleep(&pause, NULL);
        after = service_calls;
        CHECK(before == after);
        puts("PASS: stop joins the service thread and is idempotent");

        fflush(NULL);
        child = fork();
        CHECK(child >= 0);
        if (child == 0) {
                alarm(3);
                start_loop();
                stop_loop();
                _exit(0);
        }
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        puts("PASS: lifecycle starts correctly after fork");

        for (int poll_failure = 0; poll_failure < 2; poll_failure++) {
                char diagnostic_path[] = "./fuse-nfs-service-XXXXXX";
                char diagnostic_line[2048];
                char stderr_path[] = "./fuse-nfs-stderr-XXXXXX";
                FILE *diagnostic_file;
                int diagnostic_fd;
                fflush(NULL);
                diagnostic_fd = mkstemp(diagnostic_path);
                CHECK(diagnostic_fd >= 0);
                close(diagnostic_fd);
                diagnostic_fd = mkstemp(stderr_path);
                CHECK(diagnostic_fd >= 0);
                close(diagnostic_fd);
                child = fork();
                CHECK(child >= 0);
                if (child == 0) {
                        struct sync_cb_data cb = {0};
                        captured_stderr_path = stderr_path;
                        CHECK(freopen(stderr_path, "w", stderr) != NULL);
                        alarm(3);
                        logfile = diagnostic_path;
                        start_loop();
                        pthread_mutex_lock(&nfs_mutex);
                        fail_service = 1;
                        fail_poll = poll_failure;
                        pending[0] = &cb;
                        pthread_mutex_unlock(&nfs_mutex);
                        wait_for_nfs_reply(nfs, &cb);
                        _exit(99);
                }
                CHECK(waitpid(child, &status, 0) == child);
                CHECK(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE);
                diagnostic_file = fopen(diagnostic_path, "r");
                CHECK(diagnostic_file != NULL);
                CHECK(fgets(diagnostic_line, sizeof(diagnostic_line), diagnostic_file));
                CHECK(fgets(diagnostic_line, sizeof(diagnostic_line), diagnostic_file));
                CHECK(strstr(diagnostic_line, "fatal NFS service failure") != NULL);
                CHECK(strstr(diagnostic_line, "rpc_error=[injected connection failure]") != NULL);
                CHECK(strstr(diagnostic_line, "NOENT") == NULL);
                if (poll_failure) {
                        CHECK(strstr(diagnostic_line, "revents=-1 flags= poll_errno=") != NULL);
                        CHECK(strstr(diagnostic_line, strerror(EBADF)) != NULL);
                } else {
                        CHECK(strstr(diagnostic_line, "flags=POLLIN POLLERR POLLHUP ") != NULL);
                }
                fclose(diagnostic_file);
                unlink(diagnostic_path);
                diagnostic_file = fopen(stderr_path, "r");
                CHECK(diagnostic_file != NULL);
                CHECK(fgets(diagnostic_line, sizeof(diagnostic_line), diagnostic_file));
                CHECK(strstr(diagnostic_line, "rpc_error=[injected connection failure]") != NULL);
                CHECK(strstr(diagnostic_line, "NOENT") == NULL);
                if (poll_failure) {
                        CHECK(strstr(diagnostic_line, "revents=-1 flags= poll_errno=") != NULL);
                        CHECK(strstr(diagnostic_line, strerror(EBADF)) != NULL);
                } else {
                        CHECK(strstr(diagnostic_line, "flags=POLLIN POLLERR POLLHUP ") != NULL);
                }
                fclose(diagnostic_file);
                unlink(stderr_path);
        }
        check_startup_failure(0, "Failed to init context");
        check_startup_failure(1, "Failed to parse url");
        check_startup_failure(2, "Failed to mount nfs share");
        puts("PASS: startup errors are flushed before unavailable logfile I/O");
        nfs_destroy_context(nfs);
        puts("PASS: fatal service failure terminates without dangling callbacks");
        puts("PASS: all NFS service loop regression cases");
        return 0;
}
#endif
