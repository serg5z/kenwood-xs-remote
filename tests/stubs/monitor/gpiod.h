#ifndef XS8_TEST_MONITOR_GPIOD_H
#define XS8_TEST_MONITOR_GPIOD_H

/* The libgpiod 2.2 subset used by xs8_monitor.c, with the same prototypes.
 * tests/xs8_monitor_test.c implements it with scripted edge events. */
#include <stddef.h>
#include <stdint.h>

struct gpiod_chip;
struct gpiod_line_request;
struct gpiod_edge_event;
struct gpiod_edge_event_buffer;

enum gpiod_line_value {
    GPIOD_LINE_VALUE_ERROR = -1,
    GPIOD_LINE_VALUE_INACTIVE = 0,
    GPIOD_LINE_VALUE_ACTIVE = 1
};

enum gpiod_edge_event_type {
    GPIOD_EDGE_EVENT_RISING_EDGE = 1,
    GPIOD_EDGE_EVENT_FALLING_EDGE
};

enum gpiod_line_value
gpiod_line_request_get_value(struct gpiod_line_request *request,
                             unsigned int offset);
int gpiod_line_request_get_fd(struct gpiod_line_request *request);
int gpiod_line_request_read_edge_events(struct gpiod_line_request *request,
                                        struct gpiod_edge_event_buffer *buffer,
                                        size_t max_events);
enum gpiod_edge_event_type
gpiod_edge_event_get_event_type(struct gpiod_edge_event *event);
uint64_t gpiod_edge_event_get_timestamp_ns(struct gpiod_edge_event *event);
unsigned int gpiod_edge_event_get_line_offset(struct gpiod_edge_event *event);
unsigned long gpiod_edge_event_get_global_seqno(struct gpiod_edge_event *event);
unsigned long gpiod_edge_event_get_line_seqno(struct gpiod_edge_event *event);
struct gpiod_edge_event_buffer *gpiod_edge_event_buffer_new(size_t capacity);
void gpiod_edge_event_buffer_free(struct gpiod_edge_event_buffer *buffer);
struct gpiod_edge_event *
gpiod_edge_event_buffer_get_event(struct gpiod_edge_event_buffer *buffer,
                                  unsigned long index);

#endif
