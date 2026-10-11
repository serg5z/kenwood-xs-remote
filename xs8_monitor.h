#ifndef XS8_MONITOR_H
#define XS8_MONITOR_H

#include "xs8_gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

struct xs8_monitor;
enum xs8_observation {
    XS8_OBS_MATCH, XS8_OBS_MISMATCH, XS8_OBS_INVALID,
    XS8_OBS_MISSING, XS8_OBS_UNHEALTHY
};
int xs8_monitor_start(struct xs8_gpio *bus, struct xs8_monitor **monitor);
int xs8_monitor_start_manual(struct xs8_gpio *bus, struct xs8_monitor **monitor);
int xs8_monitor_poll(struct xs8_monitor *monitor, int timeout_ms);
void xs8_monitor_stop(struct xs8_monitor *monitor);
bool xs8_monitor_healthy(struct xs8_monitor *monitor);
uint64_t xs8_monitor_checkpoint(struct xs8_monitor *monitor);
enum xs8_observation xs8_monitor_verify_tx(struct xs8_monitor *monitor,
    uint64_t checkpoint, uint8_t byte, uint64_t start_ns, uint64_t end_ns,
    unsigned int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
