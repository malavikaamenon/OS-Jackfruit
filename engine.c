/*
 * MALAVIKA ARUN PES1UG24CS257
 * ADVIKA RAJ PES1UG24CS906
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Full implementation over the provided boilerplate:
 *   - UNIX domain socket control-plane IPC  (Path B)
 *   - clone() + CLONE_NEWPID/UTS/NS container isolation
 *   - pipe-based stdout/stderr capture       (Path A)
 *   - bounded-buffer producer/consumer logging pipeline
 *   - SIGCHLD / SIGINT / SIGTERM handling
 *   - ioctl integration with container_monitor LKM
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ── Constants (unchanged from boilerplate) ────────────────────────────── */
#define STACK_SIZE          (1024 * 1024)
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define LOG_DIR             "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN   256
#define LOG_CHUNK_SIZE      4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT  (40UL << 20)
#define DEFAULT_HARD_LIMIT  (64UL << 20)
#define DEVICE_NAME         "container_monitor"
#define DEVICE_PATH         "/dev/container_monitor"

/* ── Enumerations (unchanged from boilerplate) ──────────────────────────── */
typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

/* ── Data structures (boilerplate fields kept; two fields added) ────────── */
typedef struct container_record {
    char               id[CONTAINER_ID_LEN];
    pid_t              host_pid;
    time_t             started_at;
    container_state_t  state;
    unsigned long      soft_limit_bytes;
    unsigned long      hard_limit_bytes;
    int                exit_code;
    int                exit_signal;
    char               log_path[PATH_MAX];
    struct container_record *next;
    /* --- additions --- */
    int  stop_requested; /* set before we send SIGTERM/SIGKILL via `stop` */
    int  log_pipe_rfd;   /* supervisor holds the read-end of the log pipe  */
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head;
    size_t          tail;
    size_t          count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;

typedef struct {
    int  status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  log_write_fd;  
} child_config_t;

typedef struct {
    int                  server_fd;
    int                  monitor_fd;
    volatile int         should_stop;
    pthread_t            logger_thread;
    bounded_buffer_t     log_buffer;
    pthread_mutex_t      metadata_lock;
    container_record_t  *containers;
} supervisor_ctx_t;

/* Per-container producer-thread argument */
typedef struct {
    int              pipe_rfd;
    char             container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buf;
} producer_arg_t;

/* Global pointer used by async signal handlers */
static supervisor_ctx_t *g_ctx = NULL;

/* ── Helpers (provided by boilerplate, kept verbatim) ───────────────────── */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                           const char *value,
                           unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                  int argc, char *argv[], int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nv;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nv = strtol(argv[i+1], &end, 10);
            if (errno != 0 || end == argv[i+1] || *end != '\0' ||
                nv < -20 || nv > 19) {
                fprintf(stderr, "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i+1]);
                return -1;
            }
            req->nice_value = (int)nv;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ── Bounded Buffer ─────────────────────────────────────────────────────── */

static int bounded_buffer_init(bounded_buffer_t *b)
{
    int rc;
    memset(b, 0, sizeof(*b));

    rc = pthread_mutex_init(&b->mutex, NULL);
    if (rc != 0) return rc;

    rc = pthread_cond_init(&b->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&b->mutex); return rc; }

    rc = pthread_cond_init(&b->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&b->not_empty);
        pthread_mutex_destroy(&b->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *b)
{
    pthread_cond_destroy(&b->not_full);
    pthread_cond_destroy(&b->not_empty);
    pthread_mutex_destroy(&b->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->shutting_down = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
}

/*
 * bounded_buffer_push — producer-side insertion.
 *
 * Blocks on not_full while the buffer is at capacity.
 * Returns  0 on success.
 * Returns -1 if shutdown began before space was available
 *            (caller should stop producing).
 *
 * Synchronization rationale:
 *   Without the mutex two producers could pick the same tail slot
 *   and overwrite each other.  Without the cond_wait a producer
 *   would busy-spin or miss wakeups, wasting CPU or losing data.
 */
int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);

    if (b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;

    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/*
 * bounded_buffer_pop — consumer-side removal.
 *
 * Blocks on not_empty while the buffer is empty.
 * Returns  0 and fills *item when data is available.
 * Returns  1 when shutdown has been requested AND buffer is empty
 *            (caller should drain no more and exit its loop).
 *
 * Synchronization rationale:
 *   Without the mutex a consumer and a producer could corrupt
 *   head/count simultaneously.  The cond_wait avoids a busy-poll
 *   and guarantees the consumer wakes exactly when new data arrives
 *   or shutdown is broadcast.
 */
int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);

    if (b->count == 0) {   
        pthread_mutex_unlock(&b->mutex);
        return 1;
    }

    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;

    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/*
 * logging_thread — consumer.
 *
 * Drains the bounded buffer and appends each chunk to
 * logs/<container_id>.log.  Keeps looping even after shutdown
 * is requested until the buffer is fully empty so no lines are lost.
 */
void *logging_thread(void *arg)
{
    bounded_buffer_t *b = (bounded_buffer_t *)arg;
    log_item_t item;
    int rc;

    while (1) {
        rc = bounded_buffer_pop(b, &item);
        if (rc != 0)
            break;   

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            perror("logging_thread: open");
            continue;
        }

        size_t written = 0;
        while (written < item.length) {
            ssize_t n = write(fd, item.data + written, item.length - written);
            if (n < 0) {
                if (errno == EINTR) continue;
                perror("logging_thread: write");
                break;
            }
            written += (size_t)n;
        }
        close(fd);
    }
    return NULL;
}

/*
 * producer_thread — one per container.
 *
 * Reads raw bytes from the container's stdout/stderr pipe and pushes
 * LOG_CHUNK_SIZE-capped chunks into the bounded buffer.  Exits on EOF
 * (container closed its write-end) or if the buffer is shutting down.
 * The thread is created detached so the supervisor doesn't need to join it.
 */
static void *producer_thread(void *arg)
{
    producer_arg_t *pa = (producer_arg_t *)arg;
    log_item_t item;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, pa->container_id, CONTAINER_ID_LEN - 1);

    while (1) {
        ssize_t n = read(pa->pipe_rfd, item.data, LOG_CHUNK_SIZE);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;   /* EOF: container exited or all writers closed */
        }
        item.length = (size_t)n;
        if (bounded_buffer_push(pa->buf, &item) != 0)
            break;   /* buffer shutting down */
    }

    close(pa->pipe_rfd);
    free(pa);
    return NULL;
}

/*
 * child_fn — executed inside the cloned child process.
 *
 * The child runs in its own PID, UTS, and mount namespaces.
 * Steps:
 *   1. Set UTS hostname to container id.
 *   2. chroot into rootfs and chdir to /.
 *   3. Mount /proc so container-internal tools work.
 *   4. Redirect stdout+stderr to the log pipe write-end.
 *   5. Apply nice value.
 *   6. execv the requested command.
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

   
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value   = cfg->nice_value;
    int  log_write_fd = cfg->log_write_fd;

    strncpy(id,      cfg->id,      sizeof(id)      - 1); id[sizeof(id)-1] = '\0';
    strncpy(rootfs,  cfg->rootfs,  sizeof(rootfs)  - 1); rootfs[sizeof(rootfs)-1] = '\0';
    strncpy(command, cfg->command, sizeof(command) - 1); command[sizeof(command)-1] = '\0';

    /* cfg was heap-allocated by the parent; free it now that we have our copies. */
    free(cfg);
    cfg = NULL;

   
    if (sethostname(id, strlen(id)) != 0)
        perror("child_fn: sethostname (non-fatal)");

    
    if (chroot(rootfs) != 0) {
        perror("child_fn: chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("child_fn: chdir");
        return 1;
    }

   
    if (mount("proc", "/proc", "proc", 0, NULL) != 0)
        perror("child_fn: mount /proc (non-fatal)");

    /* Redirect stdout and stderr to the supervisor log pipe. */
    if (dup2(log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(log_write_fd, STDERR_FILENO) < 0) {
        perror("child_fn: dup2");
        return 1;
    }
    close(log_write_fd);

    /* Scheduling priority. */
    if (nice_value != 0)
        { int _r = nice(nice_value); (void)_r; }

    /* Split the command string into argv. */
    char cmd_copy[CHILD_COMMAND_LEN];
    strncpy(cmd_copy, command, sizeof(cmd_copy) - 1);
    cmd_copy[sizeof(cmd_copy) - 1] = '\0';

    char *argv_arr[64];
    int   ac = 0;
    char *tok = strtok(cmd_copy, " \t");
    while (tok && ac < 63) {
        argv_arr[ac++] = tok;
        tok = strtok(NULL, " \t");
    }
    argv_arr[ac] = NULL;

    if (ac == 0) {
        fprintf(stderr, "child_fn: empty command\n");
        return 1;
    }

    execv(argv_arr[0], argv_arr);
    perror("child_fn: execv");
    return 1;
}

/* ── monitor ioctl wrappers (provided by boilerplate, kept verbatim) ─────── */

int register_with_monitor(int monitor_fd,
                           const char *container_id,
                           pid_t host_pid,
                           unsigned long soft_limit_bytes,
                           unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid              = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd,
                             const char *container_id,
                             pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

/* ── Supervisor: metadata helpers ───────────────────────────────────────── */

/* Caller must hold metadata_lock. */
static container_record_t *find_container(supervisor_ctx_t *ctx,
                                           const char *id)
{
    container_record_t *c = ctx->containers;
    while (c) {
        if (strncmp(c->id, id, CONTAINER_ID_LEN) == 0)
            return c;
        c = c->next;
    }
    return NULL;
}

/* ── Supervisor: launch a container ─────────────────────────────────────── */

/*
 * launch_container — creates a pipe, calls clone(), registers the new
 * process with the kernel monitor, records metadata, and starts a
 * producer thread to forward the child's output into the log buffer.
 *
 * Returns 0 and sets *out_pid on success, -1 on any failure.
 */
static int launch_container(supervisor_ctx_t *ctx,
                              const control_request_t *req,
                              pid_t *out_pid)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("launch_container: pipe");
        return -1;
    }

   
    child_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        perror("launch_container: calloc cfg");
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }
    strncpy(cfg->id,      req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,  req->rootfs,        PATH_MAX - 1);
    strncpy(cfg->command, req->command,       CHILD_COMMAND_LEN - 1);
    cfg->nice_value   = req->nice_value;
    cfg->log_write_fd = pipefd[1];   /* child writes here */

    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("launch_container: malloc stack");
        free(cfg);
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }

    pid_t pid = clone(child_fn,
                      stack + STACK_SIZE,           /* stack grows down */
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      cfg);
    

    if (pid < 0) {
        perror("launch_container: clone");
        free(cfg); free(stack);
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }
   
    close(pipefd[1]);

    
    if (ctx->monitor_fd >= 0 &&
        register_with_monitor(ctx->monitor_fd,
                              req->container_id, pid,
                              req->soft_limit_bytes,
                              req->hard_limit_bytes) != 0)
        perror("launch_container: register_with_monitor (non-fatal)");

    /* Allocate and fill a metadata record. */
    container_record_t *rec = calloc(1, sizeof(*rec));
    if (!rec) {
        perror("launch_container: calloc");
        close(pipefd[0]);
        return -1;
    }
    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid         = pid;
    rec->started_at       = time(NULL);
    rec->state            = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->log_pipe_rfd     = pipefd[0];
    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next        = ctx->containers;
    ctx->containers  = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Start a detached producer thread for this container's pipe. */
    producer_arg_t *pa = malloc(sizeof(*pa));
    if (pa) {
        pa->pipe_rfd = pipefd[0];
        pa->buf      = &ctx->log_buffer;
        strncpy(pa->container_id, req->container_id, CONTAINER_ID_LEN - 1);

        pthread_t pt;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&pt, &attr, producer_thread, pa) != 0) {
            perror("launch_container: pthread_create producer (non-fatal)");
            free(pa);
            close(pipefd[0]);
        }
        pthread_attr_destroy(&attr);
    }

    if (out_pid) *out_pid = pid;
    return 0;
}

/* ── Signal handlers ─────────────────────────────────────────────────────── */

/*
 * sigchld_handler — reaps any exited children immediately (WNOHANG loop)
 * and updates container metadata.
 *
 * Classification rule (matches spec):
 *   stop_requested set  → CONTAINER_STOPPED
 *   SIGKILL, no request → CONTAINER_KILLED  (hard-limit kill from LKM)
 *   normal exit         → CONTAINER_EXITED
 */
static void sigchld_handler(int signo)
{
    (void)signo;
    if (!g_ctx) return;

    int wstatus;
    pid_t pid;

    while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *c = g_ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(wstatus)) {
                    c->exit_code   = WEXITSTATUS(wstatus);
                    c->exit_signal = 0;
                    c->state       = CONTAINER_EXITED;
                } else if (WIFSIGNALED(wstatus)) {
                    c->exit_signal = WTERMSIG(wstatus);
                    c->exit_code   = 128 + c->exit_signal;
                    c->state = c->stop_requested ? CONTAINER_STOPPED
                                                 : CONTAINER_KILLED;
                }
                if (g_ctx->monitor_fd >= 0)
                    unregister_from_monitor(g_ctx->monitor_fd, c->id, pid);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}

static void shutdown_handler(int signo)
{
    (void)signo;
    if (g_ctx) g_ctx->should_stop = 1;
}

/* ── Supervisor: handle one client connection ────────────────────────────── */

static void handle_client(supervisor_ctx_t *ctx, int cfd)
{
    control_request_t  req;
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    ssize_t n = recv(cfd, &req, sizeof(req), MSG_WAITALL);
    if (n != (ssize_t)sizeof(req)) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "bad request");
        send(cfd, &resp, sizeof(resp), 0);
        return;
    }

    switch (req.kind) {

    /* ── start: launch container, return immediately ─────────────── */
    case CMD_START: {
        pid_t pid;
        if (launch_container(ctx, &req, &pid) != 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "failed to launch %s", req.container_id);
        } else {
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "started %s pid=%d", req.container_id, pid);
        }
        send(cfd, &resp, sizeof(resp), 0);
        break;
    }

    /* ── run: launch then block until exit, stream final status ──── */
    case CMD_RUN: {
        pid_t pid;
        if (launch_container(ctx, &req, &pid) != 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "failed to launch %s", req.container_id);
            send(cfd, &resp, sizeof(resp), 0);
            break;
        }

        /* Interim ACK so the client knows the container started. */
        control_response_t ack;
        memset(&ack, 0, sizeof(ack));
        ack.status = 1;   /* 1 = "running, keep waiting" */
        snprintf(ack.message, sizeof(ack.message),
                 "running %s pid=%d", req.container_id, pid);
        send(cfd, &ack, sizeof(ack), 0);

        /* Block until the container exits. */
        int wstatus;
        pid_t wpid;
        do { wpid = waitpid(pid, &wstatus, 0); }
        while (wpid < 0 && errno == EINTR);

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req.container_id);
        if (c) {
            if (WIFEXITED(wstatus)) {
                c->exit_code   = WEXITSTATUS(wstatus);
                c->exit_signal = 0;
                c->state       = CONTAINER_EXITED;
            } else if (WIFSIGNALED(wstatus)) {
                c->exit_signal = WTERMSIG(wstatus);
                c->exit_code   = 128 + c->exit_signal;
                c->state = c->stop_requested ? CONTAINER_STOPPED
                                             : CONTAINER_KILLED;
            }
            resp.status = c->exit_code;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        snprintf(resp.message, sizeof(resp.message),
                 "%s exited status=%d", req.container_id, resp.status);
        if (ctx->monitor_fd >= 0)
            unregister_from_monitor(ctx->monitor_fd, req.container_id, pid);
        send(cfd, &resp, sizeof(resp), 0);
        break;
    }

    /* ── ps: list all containers ─────────────────────────────────── */
    case CMD_PS: {
        char buf[4096];
        int  off = 0;
        off += snprintf(buf + off, sizeof(buf) - off,
                        "%-20s %-8s %-10s %-10s %-10s %s\n",
                        "ID", "PID", "STATE",
                        "SOFT(MiB)", "HARD(MiB)", "EXIT");

        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c && off < (int)sizeof(buf) - 128) {
            char started[32];
            struct tm *tm = localtime(&c->started_at);
            strftime(started, sizeof(started), "%H:%M:%S", tm);

            off += snprintf(buf + off, sizeof(buf) - off,
                            "%-20s %-8d %-10s %-10lu %-10lu %d\n",
                            c->id,
                            c->host_pid,
                            state_to_string(c->state),
                            c->soft_limit_bytes >> 20,
                            c->hard_limit_bytes >> 20,
                            c->exit_code);
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        resp.status = 0;
        strncpy(resp.message, buf, sizeof(resp.message) - 1);
        send(cfd, &resp, sizeof(resp), 0);
        break;
    }

    /* ── logs: stream the log file back to the client ────────────── */
    case CMD_LOGS: {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req.container_id);
        FILE *f = fopen(path, "r");
        if (!f) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "no log for %s", req.container_id);
            send(cfd, &resp, sizeof(resp), 0);
            break;
        }

        
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message), "log:");
        send(cfd, &resp, sizeof(resp), 0);

        
        char chunk[CONTROL_MESSAGE_LEN];
        size_t bytes;
        while ((bytes = fread(chunk, 1, sizeof(chunk), f)) > 0) {
            control_response_t cr;
            memset(&cr, 0, sizeof(cr));
            cr.status = 2;   /* data chunk */
            memcpy(cr.message, chunk, bytes);
            send(cfd, &cr, sizeof(cr), 0);
        }
        fclose(f);

        /* EOF sentinel. */
        control_response_t eof;
        memset(&eof, 0, sizeof(eof));
        eof.status = 3;
        send(cfd, &eof, sizeof(eof), 0);
        break;
    }

    /* ── stop: SIGTERM → wait → SIGKILL ─────────────────────────── */
    case CMD_STOP: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req.container_id);
        if (!c || c->state != CONTAINER_RUNNING) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "%s not found or not running", req.container_id);
            send(cfd, &resp, sizeof(resp), 0);
            break;
        }
        c->stop_requested = 1;
        pid_t pid = c->host_pid;
        pthread_mutex_unlock(&ctx->metadata_lock);

        /* Graceful SIGTERM, then up to 3 s, then SIGKILL. */
        kill(pid, SIGTERM);
        int waited = 0;
        while (waited < 30) {          /* 30 × 100 ms = 3 s */
            usleep(100000);
            waited++;
            if (waitpid(pid, NULL, WNOHANG) > 0) goto reaped;
        }
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);

reaped:
        pthread_mutex_lock(&ctx->metadata_lock);
        c = find_container(ctx, req.container_id);
        if (c) c->state = CONTAINER_STOPPED;
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (ctx->monitor_fd >= 0)
            unregister_from_monitor(ctx->monitor_fd, req.container_id, pid);

        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "stopped %s", req.container_id);
        send(cfd, &resp, sizeof(resp), 0);
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "unknown command");
        send(cfd, &resp, sizeof(resp), 0);
        break;
    }
}

/* ── Supervisor main loop ────────────────────────────────────────────────── */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    /* Ensure log directory exists. */
    mkdir(LOG_DIR, 0755);

    /* Initialise mutex. */
    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { errno = rc; perror("pthread_mutex_init"); return 1; }

    
    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc; perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    
    ctx.monitor_fd = open(DEVICE_PATH, O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr,
                "[supervisor] WARNING: cannot open /dev/container_monitor: %s\n"
                "[supervisor] Memory monitoring disabled.\n",
                strerror(errno));
    else
        fprintf(stderr, "[supervisor] kernel monitor fd=%d\n", ctx.monitor_fd);

    
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); goto cleanup; }

    {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

        if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("bind"); goto cleanup;
        }
        if (listen(ctx.server_fd, 32) < 0) {
            perror("listen"); goto cleanup;
        }
    }
    
    fcntl(ctx.server_fd, F_SETFL, O_NONBLOCK);

    
    {
        struct sigaction sa;

        
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sigchld_handler;
        sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
        sigaction(SIGCHLD, &sa, NULL);

        
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = shutdown_handler;
        sa.sa_flags   = SA_RESTART;
        sigaction(SIGINT,  &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }

    
    rc = pthread_create(&ctx.logger_thread, NULL,
                        logging_thread, &ctx.log_buffer);
    if (rc != 0) {
        errno = rc; perror("pthread_create logger"); goto cleanup;
    }

    fprintf(stderr,
            "[supervisor] ready  rootfs=%s  ctrl=%s\n", rootfs, CONTROL_PATH);

    
    while (!ctx.should_stop) {
        struct sockaddr_un ca;
        socklen_t clen = sizeof(ca);
        int cfd = accept(ctx.server_fd, (struct sockaddr *)&ca, &clen);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(50000);   /* 50 ms poll */
                continue;
            }
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        handle_client(&ctx, cfd);
        close(cfd);
    }

    fprintf(stderr, "[supervisor] shutting down…\n");

    
    pthread_mutex_lock(&ctx.metadata_lock);
    {
        container_record_t *c = ctx.containers;
        while (c) {
            if (c->state == CONTAINER_RUNNING) {
                c->stop_requested = 1;
                kill(c->host_pid, SIGKILL);
            }
            c = c->next;
        }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;

cleanup:
    
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    if (ctx.logger_thread)
        pthread_join(ctx.logger_thread, NULL);

    
    pthread_mutex_lock(&ctx.metadata_lock);
    {
        container_record_t *c = ctx.containers;
        while (c) {
            container_record_t *next = c->next;
            free(c);
            c = next;
        }
        ctx.containers = NULL;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    if (ctx.server_fd  >= 0) { close(ctx.server_fd); unlink(CONTROL_PATH); }

    fprintf(stderr, "[supervisor] exited cleanly.\n");
    return 0;
}

/* ── Client: send a request and read response(s) ─────────────────────────── */

static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect — is the supervisor running?");
        close(fd);
        return 1;
    }

    if (send(fd, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send"); close(fd); return 1;
    }

    int exit_status = 0;
    control_response_t resp;

    while (1) {
        ssize_t n = recv(fd, &resp, sizeof(resp), MSG_WAITALL);
        if (n <= 0) break;

        switch (resp.status) {
        case 1:
            
            fprintf(stdout, "%s\n", resp.message);
            fflush(stdout);
            break;
        case 2:
           
            fwrite(resp.message,
                   1,
                   strnlen(resp.message, CONTROL_MESSAGE_LEN),
                   stdout);
            fflush(stdout);
            break;
        case 3:
            
            goto done;
        default:
            
            fprintf(stdout, "%s\n", resp.message);
            fflush(stdout);
            exit_status = (resp.status == 0) ? 0 : 1;
            goto done;
        }
    }

done:
    close(fd);
    return exit_status;
}

/* ── CLI command functions (boilerplate structure, now calls real IPC) ────── */

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <rootfs> <command> [opts]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs)        - 1);
    strncpy(req.command,      argv[4], sizeof(req.command)       - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <rootfs> <command> [opts]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs)        - 1);
    strncpy(req.command,      argv[4], sizeof(req.command)       - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}

