#include "xs8_gpio.h"
#include "xs8_monitor.h"

#include <chrono>
#include <cstdio>

/* Test-only backend: no device access, GPIO requests, or physical bus traffic. */
struct xs8_monitor {};

extern "C" {
int xs8_gpio_open(xs8_gpio *, bool) { return 0; }
int xs8_gpio_release(xs8_gpio *) { return 0; }
void xs8_gpio_close(xs8_gpio *) {}
int64_t xs8_gpio_now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
int xs8_gpio_send(xs8_gpio *, uint8_t byte, int64_t *start)
{
    if (start) *start = xs8_gpio_now_ns();
    std::fprintf(stderr, "FAKE_TX 0x%02X\n", byte);
    return 0;
}
int xs8_monitor_start_manual(xs8_gpio *, xs8_monitor **monitor)
{
    *monitor = new xs8_monitor;
    return 0;
}
int xs8_monitor_poll(xs8_monitor *, int) { return 0; }
void xs8_monitor_stop(xs8_monitor *monitor) { delete monitor; }
bool xs8_monitor_healthy(xs8_monitor *) { return true; }
uint64_t xs8_monitor_checkpoint(xs8_monitor *) { return 0; }
int xs8_monitor_snapshot(xs8_monitor *, uint64_t *, xs8_monitor_record *,
                         size_t, size_t *count)
{
    *count = 0;
    return 0;
}
bool xs8_monitor_partial(xs8_monitor *, uint64_t, xs8_monitor_record *)
{
    return false;
}
xs8_observation xs8_monitor_verify_tx(xs8_monitor *, uint64_t, uint8_t,
                                     uint64_t, uint64_t, unsigned int)
{
    return XS8_OBS_MATCH;
}
}
