#ifndef XS8_TEST_TX_GPIOD_H
#define XS8_TEST_TX_GPIOD_H
#include <stddef.h>
#include <stdint.h>
struct gpiod_chip;
struct gpiod_line_request;
struct gpiod_line_settings;
struct gpiod_line_config;
struct gpiod_request_config;
enum gpiod_line_value { GPIOD_LINE_VALUE_ERROR=-1, GPIOD_LINE_VALUE_INACTIVE=0, GPIOD_LINE_VALUE_ACTIVE=1 };
enum gpiod_line_direction { GPIOD_LINE_DIRECTION_AS_IS=1, GPIOD_LINE_DIRECTION_INPUT, GPIOD_LINE_DIRECTION_OUTPUT };
enum gpiod_line_edge { GPIOD_LINE_EDGE_NONE=1, GPIOD_LINE_EDGE_RISING, GPIOD_LINE_EDGE_FALLING, GPIOD_LINE_EDGE_BOTH };
enum gpiod_line_bias { GPIOD_LINE_BIAS_AS_IS=1, GPIOD_LINE_BIAS_UNKNOWN, GPIOD_LINE_BIAS_DISABLED, GPIOD_LINE_BIAS_PULL_UP, GPIOD_LINE_BIAS_PULL_DOWN };
enum gpiod_line_clock { GPIOD_LINE_CLOCK_MONOTONIC=1 };
struct gpiod_chip *gpiod_chip_open(const char *);
void gpiod_chip_close(struct gpiod_chip *);
struct gpiod_line_settings *gpiod_line_settings_new(void);
void gpiod_line_settings_free(struct gpiod_line_settings *);
int gpiod_line_settings_set_direction(struct gpiod_line_settings *, enum gpiod_line_direction);
int gpiod_line_settings_set_output_value(struct gpiod_line_settings *, enum gpiod_line_value);
int gpiod_line_settings_set_bias(struct gpiod_line_settings *, enum gpiod_line_bias);
int gpiod_line_settings_set_edge_detection(struct gpiod_line_settings *, enum gpiod_line_edge);
int gpiod_line_settings_set_event_clock(struct gpiod_line_settings *, enum gpiod_line_clock);
struct gpiod_line_config *gpiod_line_config_new(void);
void gpiod_line_config_free(struct gpiod_line_config *);
int gpiod_line_config_add_line_settings(struct gpiod_line_config *, const unsigned int *, size_t, struct gpiod_line_settings *);
struct gpiod_request_config *gpiod_request_config_new(void);
void gpiod_request_config_free(struct gpiod_request_config *);
void gpiod_request_config_set_event_buffer_size(struct gpiod_request_config *, size_t);
struct gpiod_line_request *gpiod_chip_request_lines(struct gpiod_chip *, struct gpiod_request_config *, struct gpiod_line_config *);
void gpiod_line_request_release(struct gpiod_line_request *);
int gpiod_line_request_set_value(struct gpiod_line_request *, unsigned int, enum gpiod_line_value);
enum gpiod_line_value gpiod_line_request_get_value(struct gpiod_line_request *, unsigned int);
void gpiod_request_config_set_consumer(struct gpiod_request_config *, const char *);
struct gpiod_edge_event;
struct gpiod_edge_event_buffer;
enum gpiod_edge_event_type { GPIOD_EDGE_EVENT_RISING_EDGE=1, GPIOD_EDGE_EVENT_FALLING_EDGE };
struct gpiod_edge_event_buffer *gpiod_edge_event_buffer_new(size_t);
void gpiod_edge_event_buffer_free(struct gpiod_edge_event_buffer *);
int gpiod_line_request_get_fd(struct gpiod_line_request *);
int gpiod_line_request_read_edge_events(struct gpiod_line_request *, struct gpiod_edge_event_buffer *, size_t);
struct gpiod_edge_event *gpiod_edge_event_buffer_get_event(struct gpiod_edge_event_buffer *, unsigned long);
uint64_t gpiod_edge_event_get_timestamp_ns(struct gpiod_edge_event *);
unsigned long gpiod_edge_event_get_global_seqno(struct gpiod_edge_event *);
unsigned long gpiod_edge_event_get_line_seqno(struct gpiod_edge_event *);
unsigned int gpiod_edge_event_get_line_offset(struct gpiod_edge_event *);
enum gpiod_edge_event_type gpiod_edge_event_get_event_type(struct gpiod_edge_event *);
#endif
