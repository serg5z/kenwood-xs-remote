#define _POSIX_C_SOURCE 200809L
#include "xs8_stimulus_core.h"
#include <gpiod.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;
static void on_signal(int signal_number) { interrupted = signal_number; }
static bool cancelled(void *ctx) { (void)ctx; return interrupted != 0; }
static int64_t monotonic_ns(void *ctx)
{
    (void)ctx;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return -1;
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static int wait_until(void *ctx, int64_t deadline)
{
    (void)ctx;
    const struct timespec ts = {deadline / 1000000000, deadline % 1000000000};
    for (;;) {
        if (interrupted) return ECANCELED;
        int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        if (rc != EINTR) return rc;
    }
}

/* Read-only service query, fixed executable/unit, bounded to two seconds.
 * Missing systemctl/unit/system bus, timeout and malformed results fail closed.
 * No shell, service start/stop, or environment-based executable lookup. */
static int service_inactive(void *ctx)
{
    (void)ctx;
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
              "kenwood-xs8.service", (char *)NULL);
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
                stimulus_service_state(state)) error = 0;
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

struct gpio_context {
    struct gpiod_chip *chip;
    struct gpiod_line_settings *settings;
    struct gpiod_line_config *config;
    struct gpiod_request_config *request_config;
    struct gpiod_line_request *request;
};
static int gpio_open(void *opaque)
{
    struct gpio_context *ctx = opaque;
    ctx->chip = gpiod_chip_open("/dev/gpiochip0");
    ctx->settings = gpiod_line_settings_new();
    ctx->config = gpiod_line_config_new();
    ctx->request_config = gpiod_request_config_new();
    if (!ctx->chip || !ctx->settings || !ctx->config || !ctx->request_config)
        return -1;
    if (gpiod_line_settings_set_direction(ctx->settings,
            GPIOD_LINE_DIRECTION_OUTPUT) < 0 ||
        gpiod_line_settings_set_output_value(ctx->settings,
            GPIOD_LINE_VALUE_INACTIVE) < 0) return -1;
    const unsigned int outputs[] = {STIM_DATA, STIM_BUSY};
    if (gpiod_line_config_add_line_settings(ctx->config, outputs, 2,
                                           ctx->settings) < 0) return -1;
    gpiod_request_config_set_consumer(ctx->request_config, "xs8-stimulus");
    ctx->request = gpiod_chip_request_lines(ctx->chip, ctx->request_config,
                                           ctx->config);
    return ctx->request ? 0 : -1;
}
static int gpio_set(void *opaque, unsigned int gpio, int level)
{
    struct gpio_context *ctx = opaque;
    return gpiod_line_request_set_value(ctx->request, gpio, level ?
        GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
}
static void gpio_close(void *opaque)
{
    struct gpio_context *ctx = opaque;
    if (ctx->request) gpiod_line_request_release(ctx->request);
    if (ctx->request_config) gpiod_request_config_free(ctx->request_config);
    if (ctx->config) gpiod_line_config_free(ctx->config);
    if (ctx->settings) gpiod_line_settings_free(ctx->settings);
    if (ctx->chip) gpiod_chip_close(ctx->chip);
}
static int parse_number(const char *text, unsigned int *value)
{
    char *end;
    errno = 0;
    unsigned long number = strtoul(text, &end, 10);
    if (errno || end == text || *end || text[0] == '-' || number > UINT_MAX)
        return -1;
    *value = (unsigned int)number;
    return 0;
}
int main(int argc, char **argv)
{
    struct stimulus_options options = {.frames = 16, .gap_ms = 100,
        .transitions = 320, .interval_us = 5000};
    bool frame_options = false, line_options = false, line_set = false;
    bool duration_set = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--receiver-disconnected"))
            options.disconnected_ack = true;
        else if (!strcmp(argv[i], "--hold-busy-high"))
            options.hold_busy_high = true;
        else if (!strcmp(argv[i], "--hold-busy-high-only"))
            options.hold_busy_high_only = true;
        else if (!strcmp(argv[i], "--help")) {
            printf("Usage: %s --receiver-disconnected [--frames 1..256] "
                   "[--gap-ms 1..1000]\n"
                   "   or: %s --receiver-disconnected --line data|busy "
                   "[--transitions EVEN_2..10000] [--interval-us 1000..20000]\n"
                   "      [--hold-busy-high (requires --line data)]\n"
                   "   or: %s --receiver-disconnected --hold-busy-high-only "
                   "--duration-ms 1..60000\n"
                   "Kenwood MUST be physically disconnected; service inactive.\n"
                   "Fixed repeating pattern: 00 FF 55 AA; planned run <=60 s.\n",
                   argv[0], argv[0], argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--line") && i + 1 < argc) {
            const char *line = argv[++i];
            line_set = true;
            if (!strcmp(line, "data")) options.line = STIM_DATA_ONLY;
            else if (!strcmp(line, "busy")) options.line = STIM_BUSY_ONLY;
            else { fprintf(stderr, "--line requires data or busy\n"); return 2; }
        } else if ((!strcmp(argv[i], "--frames") ||
                    !strcmp(argv[i], "--gap-ms") ||
                    !strcmp(argv[i], "--transitions") ||
                    !strcmp(argv[i], "--interval-us") ||
                    !strcmp(argv[i], "--duration-ms")) && i + 1 < argc) {
            unsigned int *value;
            if (!strcmp(argv[i], "--frames")) {
                value = &options.frames; frame_options = true;
            } else if (!strcmp(argv[i], "--gap-ms")) {
                value = &options.gap_ms; frame_options = true;
            } else if (!strcmp(argv[i], "--transitions")) {
                value = &options.transitions; line_options = true;
            } else if (!strcmp(argv[i], "--interval-us")) {
                value = &options.interval_us; line_options = true;
            } else {
                value = &options.duration_ms; duration_set = true;
            }
            if (parse_number(argv[++i], value) < 0) {
                fprintf(stderr, "Invalid numeric argument\n");
                return 2;
            }
        } else {
            fprintf(stderr, "Unknown/incomplete option: %s\n", argv[i]);
            return 2;
        }
    }
    if ((options.hold_busy_high_only &&
         (frame_options || line_options || line_set || options.hold_busy_high)) ||
        (duration_set && !options.hold_busy_high_only)) {
        fprintf(stderr, "--hold-busy-high-only requires --duration-ms alone; "
                "do not mix with frame, line or DATA-toggle hold options\n");
        return 2;
    }
    if ((options.line == STIM_COMBINED && line_options) ||
        (options.line != STIM_COMBINED && frame_options)) {
        fprintf(stderr, "Do not mix combined and single-line timing/count options\n");
        return 2;
    }
    if (stimulus_validate(&options) < 0) {
        fprintf(stderr, "REFUSED: --receiver-disconnected required; frames "
                "1..256, gap 1..1000 ms; single-line: even transitions 2..10000, "
                "interval 1000..20000 us; planned run <=60 s. "
                "--hold-busy-high requires --line data. "
                "--hold-busy-high-only requires --duration-ms 1..60000.\n");
        return 2;
    }
    struct sigaction action = {.sa_handler = on_signal};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0 ||
        sigaction(SIGHUP, &action, NULL) < 0) { perror("sigaction"); return 1; }
    if (options.hold_busy_high_only)
        fprintf(stderr, "DISCONNECTED-FIXTURE ONLY: operator acknowledged physical "
                "Kenwood disconnection. mode=BUSY_HIGH_ONLY data_tx=%u LOW "
                "busy_tx=%u HIGH duration_ms=%u. No RX sensing or RT scheduling.\n",
                STIM_DATA, STIM_BUSY, options.duration_ms);
    else if (options.line == STIM_COMBINED)
        fprintf(stderr, "DISCONNECTED-FIXTURE ONLY: operator acknowledged physical "
            "Kenwood disconnection. Pattern=00,FF,55,AA frames=%u gap_ms=%u. "
            "No RX sensing, bus arbitration or RT scheduling.\n",
            options.frames, options.gap_ms);
    else
        fprintf(stderr, "DISCONNECTED-FIXTURE ONLY: operator acknowledged physical "
                "Kenwood disconnection. mode=%s line=%s data_tx=%u busy_tx=%u "
                "transitions=%u interval_us=%u other TX requested %s. "
                "No RX sensing or RT scheduling.\n",
                options.hold_busy_high ? "DATA_WITH_BUSY_HIGH" : "SINGLE_LINE",
                options.line == STIM_DATA_ONLY ? "data" : "busy",
                STIM_DATA, STIM_BUSY, options.transitions, options.interval_us,
                options.hold_busy_high ? "HIGH" : "LOW");
    struct gpio_context gpio = {0};
    const struct stimulus_ops ops = {&gpio, service_inactive, gpio_open, gpio_set,
                                    monotonic_ns, wait_until, cancelled, gpio_close};
    struct stimulus_report report;
    int result = stimulus_run(&options, &ops, &report);
    int error = errno;
    /* All GPIO resources have been released before diagnostics/output. */
    if (options.hold_busy_high_only) {
        puts("gpio,level,write_ns,done_ns,gpio_result");
        if (report.busy_high_attempted)
            printf("%u,1,%" PRId64 ",%" PRId64 ",%s\n", STIM_BUSY,
                report.busy_high_write_ns, report.busy_high_done_ns,
                report.busy_high_written ? "TX_OK" : "NOT_COMPLETED");
        if (report.cleanup_attempted)
            printf("%u,0,%" PRId64 ",%" PRId64 ",%s\n", STIM_BUSY,
                report.busy_low_write_ns, report.busy_low_done_ns,
                report.busy_low_written ? "TX_OK" : "NOT_COMPLETED");
        fprintf(stderr, "BUSY_ONLY duration_ms=%u hold_complete=%s "
                "deadline_ns=%" PRId64 " wake_ns=%" PRId64 "\n",
                options.duration_ms, report.busy_hold_completed ? "yes" : "no",
                report.busy_hold_deadline_ns, report.busy_hold_wake_ns);
    } else if (options.line != STIM_COMBINED) {
        if (options.hold_busy_high)
            fprintf(stderr, "BUSY_HOLD mode=DATA_WITH_BUSY_HIGH data_tx=%u busy_tx=%u "
                    "high_attempted=%s high_result=%s high_write_ns=%" PRId64
                    " high_done_ns=%" PRId64 " lead_wake_ns=%" PRId64
                    " cleanup_low_result=%s cleanup_low_write_ns=%" PRId64 "\n",
                    STIM_DATA, STIM_BUSY,
                    report.busy_high_attempted ? "yes" : "no",
                    report.busy_high_written ? "TX_OK" : "NOT_COMPLETED",
                    report.busy_high_write_ns, report.busy_high_done_ns,
                    report.busy_lead_wake_ns,
                    report.busy_low_written ? "TX_OK" : "NOT_COMPLETED",
                    report.busy_low_write_ns);
        puts("transition,gpio,level,write_ns,deadline_ns,wake_ns,gpio_result,wait_result");
        for (unsigned int i = 0; i < report.transition_attempted; ++i)
            printf("%u,%u,%u,%" PRId64 ",%" PRId64 ",%" PRId64 ",%s,%s\n",
                i + 1, options.line == STIM_DATA_ONLY ? STIM_DATA : STIM_BUSY,
                i % 2 == 0, report.transitions[i].write_ns,
                report.transitions[i].deadline_ns, report.transitions[i].wake_ns,
                report.transitions[i].written ? "TX_OK" : "NOT_COMPLETED",
                report.transitions[i].wake_ns ? "WAIT_OK" : "NOT_COMPLETED");
        fprintf(stderr, "SINGLE_LINE requested=%u attempted=%u written=%u "
                "run_complete=%s max_late_ns=%" PRId64
                " over_500us=%u over_1000us=%u samples=%u\n",
                options.transitions, report.transition_attempted,
                report.transition_written, report.single.completed ? "yes" : "no",
                report.single.max_late_ns, report.single.over_500us,
                report.single.over_1000us, report.single.samples);
    } else {
    puts("frame,byte,start_ns,end_ns,result,max_late_ns,over_500us,over_1000us,samples");
    for (unsigned int i = 0; i < report.attempted; ++i) {
        const struct stimulus_frame *frame = &report.frames[i];
        printf("%u,0x%02X,%" PRId64 ",%" PRId64 ",%s,%" PRId64 ",%u,%u,%u\n",
            i + 1, frame->byte, frame->start_ns, frame->end_ns,
            frame->completed ? "TX_OK" : "TX_FAIL", frame->max_late_ns,
            frame->over_500us, frame->over_1000us, frame->samples);
    }
    }
    if (options.hold_busy_high_only)
        fprintf(stderr, "STIMULUS mode=BUSY_HIGH_ONLY data_tx=%u busy_tx=%u "
                "cleanup=%s result=%s\n", STIM_DATA, STIM_BUSY,
                !report.cleanup_attempted ? "NOT_REQUESTED" :
                report.cleanup_ok ? "LOW_WRITES_OK" : "FAILED",
                result == 0 ? "TX_OK" : strerror(error));
    else if (options.line == STIM_COMBINED)
        fprintf(stderr, "STIMULUS completed=%u attempted=%u cleanup=%s result=%s\n",
            report.completed, report.attempted, !report.cleanup_attempted ?
            "NOT_REQUESTED" : report.cleanup_ok ? "LOW_WRITES_OK" : "FAILED",
            result == 0 ? "TX_OK" : strerror(error));
    else
        fprintf(stderr, "STIMULUS transitions_written=%u cleanup=%s result=%s\n",
            report.transition_written, !report.cleanup_attempted ?
            "NOT_REQUESTED" : report.cleanup_ok ? "LOW_WRITES_OK" : "FAILED",
            result == 0 ? "TX_OK" : strerror(error));
    if (report.cleanup_attempted && !report.cleanup_ok)
        fprintf(stderr, "WARNING: failed to drive both TX outputs LOW; "
                "lines released, electrical levels unverified.\n");
    if (result < 0 && !report.attempted && !report.transition_attempted &&
        !report.busy_high_attempted)
        fprintf(stderr, "Check loaded/inactive kenwood-xs8.service, exclusive "
                "GPIO6/19 ownership and permissions. No service changes made.\n");
    if (fflush(stdout) == EOF || ferror(stderr)) return 1;
    if (report.cleanup_attempted && !report.cleanup_ok) return 1;
    return interrupted ? 128 + interrupted : result == 0 ? 0 : 1;
}
