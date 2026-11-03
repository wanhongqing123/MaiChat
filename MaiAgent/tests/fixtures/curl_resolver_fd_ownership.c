// 文件作用：使用实际 curl resolver 函数验证 fd 所有权；只操作本进程的 eventfd/pipe 与 /dev/null。
// 抽取函数由构建脚本插入；首次 close 后立即 dup2 复用 fd，确定性暴露第二次错误关闭。
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#if FIXTURE_EVENTFD
#define USE_EVENTFD 1
#endif
#define CURL_SOCKET_BAD (-1)
#define CURL_ASYNC_SUCCESS 0
#define FALSE false
#define curl_thread_t_null 0
typedef int curl_socket_t;
typedef int curl_mutex_t;
typedef intptr_t curl_thread_t;
struct addrinfo;
struct Curl_easy {
    int unused;
};
struct thread_data;
struct thread_sync_data {
    curl_mutex_t* mtx;
    int done, port;
    char* hostname;
    struct Curl_easy* data;
    curl_socket_t sock_pair[2];
    int sock_error;
    void* res;
    struct thread_data* td;
};
struct thread_data {
    curl_thread_t thread_hnd;
    struct thread_sync_data tsd;
};
struct Curl_async {
    struct thread_data* tdata;
    char* hostname;
};

static int watched = -1, replacement = -1, closes, notified = -1;
static bool fail_malloc, fail_strdup, require_notification, wrong_order;
static void Curl_mutex_init(curl_mutex_t* mutex) {
    *mutex = 0;
}
static void Curl_mutex_destroy(curl_mutex_t* mutex) {
    (void)mutex;
}
static void Curl_mutex_acquire(curl_mutex_t* mutex) {
    (void)mutex;
}
static void Curl_mutex_release(curl_mutex_t* mutex) {
    (void)mutex;
}
static void Curl_freeaddrinfo(void* addresses) {
    free(addresses);
}
static void Curl_thread_destroy(curl_thread_t thread) {
    (void)thread;
}
static void Curl_thread_join(curl_thread_t* thread) {
    *thread = 0;
}
static void Curl_multi_closed(struct Curl_easy* easy, int descriptor) {
    (void)easy;
    notified = descriptor;
}
static void* fixture_malloc(size_t size) {
    return fail_malloc ? NULL : malloc(size);
}
static char* fixture_strdup(const char* text) {
    if (fail_strdup) return NULL;
    size_t size = strlen(text) + 1;
    char* result = malloc(size);
    if (result) memcpy(result, text, size);
    return result;
}
static int fixture_create(int descriptors[2], bool nonblocking) {
    (void)nonblocking;
#ifdef USE_EVENTFD
    int fd = eventfd(0, EFD_CLOEXEC);
    if (fd < 0) return -1;
    descriptors[0] = descriptors[1] = fd;
#else
    if (pipe(descriptors)) return -1;
#endif
    watched = descriptors[0];
    return 0;
}
static int fixture_close(int descriptor) {
    closes++;
    if (descriptor == watched && require_notification && notified != descriptor) wrong_order = true;
    int result = close(descriptor);
    if (descriptor == watched && result == 0 && replacement == -1) {
        int new_owner = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (new_owner < 0) {
            perror("open fixture owner");
            exit(2);
        }
        // dup2 明确将同一 fd 数字转给新对象；后续错误 close 会破坏这个新所有者。
        if (dup2(new_owner, watched) < 0) {
            perror("dup2 fixture owner");
            exit(2);
        }
        if (new_owner != watched) close(new_owner);
        replacement = watched;
    }
    return result;
}
#define malloc fixture_malloc
#define strdup fixture_strdup
#define wakeup_create fixture_create
#define wakeup_close fixture_close

/* @CURL_FUNCTIONS@ */

#undef malloc
#undef strdup

static void reset_fixture(void) {
    watched = replacement = notified = -1;
    closes = 0;
    fail_malloc = fail_strdup = require_notification = wrong_order = false;
}
static int verify(const char* name, int expected_closes) {
    bool alive = replacement >= 0 && fcntl(replacement, F_GETFD) >= 0;
    bool success = alive && closes == expected_closes && !wrong_order;
    printf("mode=%s case=%s closes=%d replacement_alive=%d notification_order=%d result=%s\n",
           FIXTURE_EVENTFD ? "eventfd" : "pipe", name, closes, alive, !wrong_order,
           success ? "PASS" : "FAIL");
    if (alive) close(replacement);
    return success ? 0 : 1;
}
static int completed_resolve(bool detached) {
    reset_fixture();
    struct thread_data* td = calloc(1, sizeof(*td));
    if (!td || !init_thread_sync_data(td, "fixture.invalid", 443, NULL)) return 2;
    td->tsd.done = detached ? 0 : 1;
    td->thread_hnd = detached ? 1 : curl_thread_t_null;
    struct Curl_async async = {td, NULL};
    require_notification = true;
    destroy_async_data(&async);
    if (detached) {
        destroy_thread_sync_data(&td->tsd);
        free(td);
    }
    return verify(detached ? "detached-worker-cleanup" : "completed-resolve",
                  FIXTURE_EVENTFD ? 1 : 2);
}
static int failed_hostname_allocation(void) {
    reset_fixture();
    fail_strdup = true;
    struct thread_data* td = calloc(1, sizeof(*td));
    if (!td || init_thread_sync_data(td, "fixture.invalid", 443, NULL)) return 2;
    free(td);
    return verify("hostname-allocation-failure", FIXTURE_EVENTFD ? 1 : 2);
}
static int failed_early_allocation(void) {
    reset_fixture();
    int original_stdin = dup(STDIN_FILENO);
    int controlled = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (controlled < 0 || dup2(controlled, STDIN_FILENO) < 0) return 2;
    if (controlled != STDIN_FILENO) close(controlled);
    watched = STDIN_FILENO;
    fail_malloc = true;
    struct thread_data* td = calloc(1, sizeof(*td));
    if (!td || init_thread_sync_data(td, "fixture.invalid", 443, NULL)) return 2;
    free(td);
    bool alive = fcntl(STDIN_FILENO, F_GETFD) >= 0;
    bool success = closes == 0 && alive;
    printf("mode=%s case=early-allocation-failure closes=%d unowned_stdin_alive=%d result=%s\n",
           FIXTURE_EVENTFD ? "eventfd" : "pipe", closes, alive, success ? "PASS" : "FAIL");
    if (original_stdin >= 0) {
        dup2(original_stdin, STDIN_FILENO);
        close(original_stdin);
    } else if (alive)
        close(STDIN_FILENO);
    return success ? 0 : 1;
}
int main(void) {
    int failures = completed_resolve(false);
    failures += completed_resolve(true);
    failures += failed_hostname_allocation();
    failures += failed_early_allocation();
    return failures ? 1 : 0;
}
