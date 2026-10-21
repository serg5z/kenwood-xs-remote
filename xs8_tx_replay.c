#define _POSIX_C_SOURCE 200809L
#include "xs8_gpio.h"
#include "xs8_tx_replay_core.h"
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;
static void on_signal(int number) { interrupted = number; }
static bool cancelled(void *ctx) { (void)ctx; return interrupted != 0; }
static int64_t monotonic_ns(void *ctx) { (void)ctx; return xs8_gpio_now_ns(); }
static int wait_until(void *ctx, int64_t deadline)
{
    (void)ctx;
    const struct timespec ts = {deadline / 1000000000, deadline % 1000000000};
    for (;;) {
        if (interrupted) return ECANCELED;
        int error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        if (error != EINTR) return error;
    }
}

/* Bounded, read-only systemctl query. Never changes service state. */
static int unit_inactive(const char *unit)
{
    int64_t start = monotonic_ns(NULL);
    if (start < 0) return -1;
    int fd[2];
    if (pipe(fd) < 0) return -1;
    if (fcntl(fd[0], F_SETFL, O_NONBLOCK) < 0) {
        int error = errno;
        close(fd[0]); close(fd[1]);
        errno = error;
        return -1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(fd[0]);
        if (fd[1] != STDOUT_FILENO) {
            if (dup2(fd[1], STDOUT_FILENO) < 0) _exit(127);
            close(fd[1]);
        }
        execl("/usr/bin/systemctl", "systemctl", "--system", "--no-pager",
              "show", "--property=LoadState", "--property=ActiveState",
              unit, (char *)NULL);
        _exit(127);
    }
    close(fd[1]);
    if (pid < 0) { close(fd[0]); return -1; }
    char state[128] = {0};
    size_t used = 0;
    bool reaped = false, eof = false, overflow = false;
    int status = 0, error = EACCES;
    while (!interrupted) {
        char chunk[128];
        ssize_t n = read(fd[0], chunk, sizeof(chunk));
        if (n > 0) {
            if ((size_t)n >= sizeof(state) - used) overflow = true;
            else { memcpy(state + used, chunk, (size_t)n); used += (size_t)n; }
        } else if (n == 0) eof = true;
        else if (errno != EAGAIN && errno != EINTR) { error = errno; break; }
        if (!reaped) {
            pid_t done = waitpid(pid, &status, WNOHANG);
            if (done == pid) reaped = true;
            else if (done < 0 && errno != EINTR) { error = errno; break; }
        }
        if (reaped && eof) {
            if (!overflow && strlen(state) == used &&
                WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                replay_service_state(state)) error = 0;
            break;
        }
        int64_t now = monotonic_ns(NULL);
        if (now < 0) { error = errno; break; }
        if (now - start >= 2000000000) { error = ETIMEDOUT; break; }
        /* Also bound polling when the helper closes stdout before exiting. */
        struct pollfd pfd = {.fd = eof ? -1 : fd[0], .events = POLLIN};
        if (poll(&pfd, 1, 10) < 0 && errno != EINTR) { error = errno; break; }
    }
    if (interrupted) error = ECANCELED;
    close(fd[0]);
    if (!reaped) {
        (void)kill(pid, SIGKILL); /* Only our read-only query child. */
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    if (error) { errno = error; return -1; }
    return 0;
}

static int services_inactive(void *ctx)
{
    (void)ctx;
    if (unit_inactive("kenwood-xs8.service") < 0) return -1;
    return unit_inactive("kenwood-cec-xs-client.service");
}
static int open_tx_only(void *ctx) { return xs8_gpio_open_tx_only(ctx); }
static int send_frame(void *ctx, uint8_t byte, int64_t *start)
{ return xs8_gpio_send(ctx, byte, start); }
static int release_tx(void *ctx) { return xs8_gpio_release(ctx); }
static void close_tx(void *ctx) { xs8_gpio_close(ctx); }

static int parse_gap(const char *text, unsigned int *value)
{
    if (!*text || strspn(text, "0123456789") != strlen(text)) return -1;
    char *end;
    errno = 0;
    unsigned long n = strtoul(text, &end, 10);
    if (errno || *end || n > XS8_REPLAY_MAX_GAP_MS) return -1;
    *value = (unsigned int)n;
    return 0;
}
static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s [--dry-run | --receiver-disconnected] "
        "[--direction down|up] [--entry-start-ms 0..5000] "
        "[--start-stop-ms 0..5000] [--stop-root-ms 0..5000]\n"
        "One sequence only; default DOWN, zero added gaps. TX GPIO6/19 only.\n"
        "Kenwood MUST be disconnected. Both production services must be inactive.\n",
        program);
}
int main(int argc, char **argv)
{
    struct replay_options options = {0};
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--receiver-disconnected")) options.disconnected = true;
        else if (!strcmp(argv[i], "--dry-run")) options.dry_run = true;
        else if (!strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else if (!strcmp(argv[i], "--direction") && i + 1 < argc) {
            const char *direction = argv[++i];
            if (strcmp(direction, "up") && strcmp(direction, "down")) goto invalid;
            options.up = !strcmp(direction, "up");
        } else {
            int gap = !strcmp(argv[i], "--entry-start-ms") ? 0 :
                      !strcmp(argv[i], "--start-stop-ms") ? 1 :
                      !strcmp(argv[i], "--stop-root-ms") ? 2 : -1;
            if (gap < 0 || i + 1 >= argc || parse_gap(argv[++i], &options.gap_ms[gap]))
                goto invalid;
        }
    }
    if (!options.dry_run && !options.disconnected) {
        fprintf(stderr, "REFUSED: --receiver-disconnected is mandatory for TX.\n");
        return 2;
    }
    printf("mode=%s direction=%s DATA_TX=6 BUSY_TX=19 RX=NONE BUSY_SENSING=DISABLED "
           "gap_origin=previous_send_return minimum_settle_us=500\n",
           options.dry_run ? "DRY_RUN" : "DISCONNECTED_TX_ONLY",
           options.up ? "UP" : "DOWN");
    printf("sequence=0x17,0x%02X,0x09,0x07 additional_gaps_ms=%u,%u,%u "
           "historical_timing=UNKNOWN initial_control_experiment=1\n",
           replay_byte(&options, 1), options.gap_ms[0], options.gap_ms[1], options.gap_ms[2]);
    fflush(stdout);
    if (options.dry_run) return 0; /* No GPIO or service query. */
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0) {
        perror("signal setup"); return 1;
    }
    struct xs8_gpio bus = {0};
    const struct replay_ops ops = {&bus, services_inactive, open_tx_only,
        send_frame, release_tx, close_tx, monotonic_ns, wait_until, cancelled};
    struct replay_report report;
    int error = replay_run(&options, &ops, &report);
    /* Report after replay: no additional formatting in inter-frame gaps. */
    printf("index,byte,invoked_ns,tx_start_ns,completion_ns,gpio_tx,error\n");
    for (unsigned int i = 0; i < report.attempted; ++i) {
        const struct replay_frame *f = &report.frames[i];
        printf("%u,0x%02X,%" PRId64 ",%" PRId64 ",%" PRId64 ",%s,%d\n",
               i, f->byte, f->invoked_ns, f->start_ns, f->completed_ns,
               f->gpio_ok ? "TX_OK" : "TX_FAIL", f->error);
    }
    fprintf(stderr, "replay attempted=%u/4 result=%s cleanup=%s "
                    "waveform=NOT_VERIFIED receiver_execution=UNVERIFIED\n",
            report.attempted, error ? strerror(error) : "TX_COMPLETE",
            !report.cleanup_attempted ? "NOT_OPENED" : report.cleanup_ok ? "LOW_OK" : "LOW_FAILED");
    if (report.cleanup_attempted && !report.cleanup_ok)
        fprintf(stderr, "WARNING: TX LOW cleanup failed; inspect disconnected fixture.\n");
    return error ? 1 : 0;
invalid:
    usage(argv[0]);
    return 2;
}
