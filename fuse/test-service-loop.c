/* This test uses assertions with side effects; keep them active in all builds. */
#undef NDEBUG
/* Exercise the real FUSE event loop without a server or a FUSE mount. */
#ifdef WIN32
int main(void) { return 77; } /* POSIX signals and fork are used by this test. */
#else
#define FUSE_USE_VERSION 26
#define _FILE_OFFSET_BITS 64
#include <assert.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include <time.h>
#include <nfsc/libnfs.h>

static int test_get_fd(struct nfs_context *context);
static int test_which_events(struct nfs_context *context);
static int test_service(struct nfs_context *context, int revents);
static const char *test_get_error(struct nfs_context *context);
static int test_poll(struct pollfd *fds, nfds_t count, int timeout);

#define main fuse_nfs_program_main
#define nfs_get_fd test_get_fd
#define nfs_which_events test_which_events
#define nfs_service test_service
#define nfs_get_error test_get_error
#define poll test_poll
#include "fuse-nfs.c"
#undef main
#undef nfs_get_fd
#undef nfs_which_events
#undef nfs_service
#undef nfs_get_error
#undef poll

#define WORKERS 8
#define ROUNDS 200
static int server_pipe[2];
static int service_calls, completions, fail_service;
static pthread_t service_owner;
static int have_owner;
static struct sync_cb_data *pending[WORKERS];
static int interrupt_next, interrupts;

static int test_poll(struct pollfd *fds, nfds_t count, int timeout)
{
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
        return poll(fds, count, timeout);
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
        return "injected fatal service error";
}

static int test_service(struct nfs_context *context, int revents)
{
        (void)context;
        assert(revents == 0); /* EINTR must not be reported as a socket error. */
        assert(pthread_mutex_trylock(&nfs_mutex) == EBUSY);
        if (!have_owner) {
                service_owner = pthread_self();
                have_owner = 1;
        }
        assert(pthread_equal(service_owner, pthread_self()));
        service_calls++;
        if (fail_service)
                return -1;
        for (int i = 0; i < WORKERS; i++) {
                if (pending[i]) {
                        /* Publish the flag first, just like the real callbacks. */
                        pending[i]->is_finished = 1;
                        pending[i]->status = 1000 + i;
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
        assert(pipe(server_pipe) == 0);
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
                assert(pending[id] == NULL);
                pending[id] = &cb;
                pthread_mutex_unlock(&nfs_mutex);
                wait_for_nfs_reply(nfs, &cb);
                assert(cb.status == 1000 + id);
                assert(result == 2000 + id);
        }
        return NULL;
}

int main(void)
{
        pthread_t workers[WORKERS];
        int ids[WORKERS];
        int before, after, status;
        pid_t child;
        struct timespec pause = {0, 400000000};

        alarm(20);
        start_loop();
        nanosleep(&pause, NULL);
        pthread_mutex_lock(&nfs_mutex);
        assert(service_calls >= 2);
        before = service_calls;
        pthread_mutex_unlock(&nfs_mutex);
        puts("PASS: NFS service runs while FUSE is idle");

        pthread_mutex_lock(&nfs_mutex);
        interrupt_next = 1;
        pthread_mutex_unlock(&nfs_mutex);
        nanosleep(&pause, NULL);
        pthread_mutex_lock(&nfs_mutex);
        assert(service_calls > before);
        assert(interrupts == 1);
        pthread_mutex_unlock(&nfs_mutex);
        puts("PASS: interrupted poll keeps the service loop alive");

        for (int i = 0; i < WORKERS; i++) {
                ids[i] = i;
                assert(pthread_create(&workers[i], NULL, request_worker, &ids[i]) == 0);
        }
        for (int i = 0; i < WORKERS; i++)
                assert(pthread_join(workers[i], NULL) == 0);
        assert(completions == WORKERS * ROUNDS);
        puts("PASS: 1600 concurrent replies, one service owner, complete results");

        {
                struct sync_cb_data cb = { .is_finished = 1, .status = 42 };
                wait_for_nfs_reply(nfs, &cb);
                assert(cb.status == 42);
        }
        puts("PASS: reply completed before wait is not lost");
        stop_loop();
        before = service_calls;
        nanosleep(&pause, NULL);
        after = service_calls;
        assert(before == after);
        puts("PASS: stop joins the service thread and is idempotent");

        fflush(NULL);
        child = fork();
        assert(child >= 0);
        if (child == 0) {
                alarm(3);
                start_loop();
                stop_loop();
                _exit(0);
        }
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        puts("PASS: lifecycle starts correctly after fork");

        fflush(NULL);
        child = fork();
        assert(child >= 0);
        if (child == 0) {
                struct sync_cb_data cb = {0};
                alarm(3);
                start_loop();
                pthread_mutex_lock(&nfs_mutex);
                fail_service = 1;
                pending[0] = &cb;
                pthread_mutex_unlock(&nfs_mutex);
                wait_for_nfs_reply(nfs, &cb);
                _exit(99);
        }
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE);
        puts("PASS: fatal service failure terminates without dangling callbacks");
        puts("PASS: all NFS service loop regression cases");
        return 0;
}
#endif
