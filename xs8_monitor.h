#ifndef XS8_MONITOR_H
#define XS8_MONITOR_H

#include "xs8_decode.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xs8_monitor;
struct xs8_gpio;
struct xs8_monitor_record {
    uint64_t serial;
    uint64_t end_ns;
    struct xs8_decoded decoded;
};
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
/* Copies completed frames/gaps after cursor without consuming the history.
 * Returns -1 with EOVERFLOW if the bounded history has wrapped. */
int xs8_monitor_snapshot(struct xs8_monitor *monitor, uint64_t *cursor,
    struct xs8_monitor_record *records, size_t capacity, size_t *count);
/* Manual-poll mode only: expose an unfinished frame at a query deadline. */
bool xs8_monitor_partial(struct xs8_monitor *monitor, uint64_t now_ns,
    struct xs8_monitor_record *record);
enum xs8_observation xs8_monitor_verify_tx(struct xs8_monitor *monitor,
    uint64_t checkpoint, uint8_t byte, uint64_t start_ns, uint64_t end_ns,
    unsigned int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
