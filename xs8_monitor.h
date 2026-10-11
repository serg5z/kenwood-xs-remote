#ifndef XS8_MONITOR_H
#define XS8_MONITOR_H

#include "xs8_gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

struct xs8_monitor;
int xs8_monitor_start(struct xs8_gpio *bus, struct xs8_monitor **monitor);
void xs8_monitor_stop(struct xs8_monitor *monitor);

#ifdef __cplusplus
}
#endif

#endif
