/* Real GPIO initializer/transmitter with fake libgpiod, clock and RT. No hardware. */
#define _POSIX_C_SOURCE 200809L
#include <time.h>
int fake_clock_gettime(clockid_t, struct timespec *);
int fake_clock_nanosleep(clockid_t, int, const struct timespec *, struct timespec *);
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1 /* Fake sleep only; macOS has no clock_nanosleep. */
#endif
#define clock_gettime fake_clock_gettime
#define clock_nanosleep fake_clock_nanosleep
#include "../xs8_gpio.c"
#include <assert.h>

struct gpiod_chip { int unused; };
struct gpiod_line_settings { bool output; enum gpiod_line_bias bias; };
struct gpiod_line_config { unsigned int pins[2]; size_t count; bool output; enum gpiod_line_bias bias; };
struct gpiod_request_config { int unused; };
struct gpiod_line_request { bool output; };
static struct gpiod_chip chip;
static struct gpiod_line_request requests[2];
static unsigned int requested, reads, writes, sleeps, requested_pins[4], npins;
static unsigned int levels[64], fail_pin, fail_write;
static bool fail_request, fail_rx_request, fail_bias, busy_high;
static unsigned int bias_calls, allocations, frees, releases, chip_closes;
static int64_t now_ns, first_deadline, last_deadline;
int fake_clock_gettime(clockid_t c, struct timespec *t)
{ (void)c; now_ns += 100000; t->tv_sec=now_ns/1000000000; t->tv_nsec=now_ns%1000000000; return 0; }
int fake_clock_nanosleep(clockid_t c, int flags, const struct timespec *t, struct timespec *remain)
{
    (void)c; (void)flags; (void)remain;
    int64_t deadline=(int64_t)t->tv_sec*1000000000+t->tv_nsec;
    if (!sleeps++) first_deadline=deadline;
    last_deadline=deadline; now_ns=deadline; return 0;
}
struct gpiod_chip *gpiod_chip_open(const char *name) { assert(!strcmp(name,"/dev/gpiochip0")); return &chip; }
void gpiod_chip_close(struct gpiod_chip *c) { assert(c==&chip); ++chip_closes; }
struct gpiod_line_settings *gpiod_line_settings_new(void) {
    struct gpiod_line_settings *s=calloc(1,sizeof(*s)); assert(s);
    s->bias=GPIOD_LINE_BIAS_AS_IS; ++allocations; return s;
}
void gpiod_line_settings_free(struct gpiod_line_settings *s) { assert(s); ++frees; free(s); }
int gpiod_line_settings_set_direction(struct gpiod_line_settings *s, enum gpiod_line_direction d) { s->output=d==GPIOD_LINE_DIRECTION_OUTPUT; return 0; }
int gpiod_line_settings_set_output_value(struct gpiod_line_settings *s, enum gpiod_line_value v) { (void)s; assert(v==0); return 0; }
int gpiod_line_settings_set_bias(struct gpiod_line_settings *s, enum gpiod_line_bias b) {
    assert(!s->output && b==GPIOD_LINE_BIAS_DISABLED); ++bias_calls;
    if (fail_bias) { errno=ENOTSUP; return -1; } s->bias=b; return 0;
}
int gpiod_line_settings_set_edge_detection(struct gpiod_line_settings *s, enum gpiod_line_edge e) { (void)s; assert(e==GPIOD_LINE_EDGE_BOTH); return 0; }
int gpiod_line_settings_set_event_clock(struct gpiod_line_settings *s, enum gpiod_line_clock c) { (void)s; assert(c==GPIOD_LINE_CLOCK_MONOTONIC); return 0; }
struct gpiod_line_config *gpiod_line_config_new(void) { ++allocations; return calloc(1,sizeof(struct gpiod_line_config)); }
void gpiod_line_config_free(struct gpiod_line_config *c) { assert(c); ++frees; free(c); }
int gpiod_line_config_add_line_settings(struct gpiod_line_config *c, const unsigned int *pins, size_t n, struct gpiod_line_settings *s)
{ assert(n<=2); memcpy(c->pins,pins,n*sizeof(*pins)); c->count=n; c->output=s->output; c->bias=s->bias; return 0; }
struct gpiod_request_config *gpiod_request_config_new(void) { ++allocations; return calloc(1,sizeof(struct gpiod_request_config)); }
void gpiod_request_config_free(struct gpiod_request_config *c) { assert(c); ++frees; free(c); }
void gpiod_request_config_set_event_buffer_size(struct gpiod_request_config *c, size_t size) { (void)c; assert(size==4096); }
struct gpiod_line_request *gpiod_chip_request_lines(struct gpiod_chip *c, struct gpiod_request_config *r, struct gpiod_line_config *cfg)
{
    (void)c; (void)r;
    assert(cfg->bias==(cfg->output ? GPIOD_LINE_BIAS_AS_IS : GPIOD_LINE_BIAS_DISABLED));
    if (fail_request || (fail_rx_request && !cfg->output)) { errno=EBUSY; return NULL; }
    assert(requested<2);
    for(size_t i=0;i<cfg->count;++i) requested_pins[npins++]=cfg->pins[i];
    requests[requested].output=cfg->output;
    return &requests[requested++];
}
void gpiod_line_request_release(struct gpiod_line_request *r) { assert(r); ++releases; }
int gpiod_line_request_set_value(struct gpiod_line_request *r, unsigned int pin, enum gpiod_line_value v)
{
    assert(r->output && (pin==6 || pin==19)); ++writes;
    if (pin==fail_pin || writes==fail_write) { errno=EIO; return -1; }
    levels[pin]=(unsigned int)v; return 0;
}
enum gpiod_line_value gpiod_line_request_get_value(struct gpiod_line_request *r, unsigned int pin)
{ assert(!r->output && pin==26); ++reads; return busy_high ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE; }
int xs8_rt_parse_priority(const char *s) { (void)s; return 0; }
int xs8_rt_begin(struct xs8_rt_guard *g, int p, const struct xs8_rt_ops *o) { (void)g; (void)p; (void)o; return 0; }
int xs8_rt_end(struct xs8_rt_guard *g, const struct xs8_rt_ops *o) { (void)g; (void)o; return 0; }
static void reset(void)
{
    requested=reads=writes=sleeps=npins=0; fail_pin=fail_write=0; fail_request=fail_rx_request=fail_bias=busy_high=false;
    bias_calls=allocations=frees=releases=chip_closes=0;
    now_ns=1000000000; first_deadline=last_deadline=0; memset(levels,0,sizeof(levels));
}
int main(void)
{
    struct xs8_gpio bus;
    reset(); assert(xs8_gpio_open_tx_only(&bus)==0);
    assert(bus.tx_only && !bus.rx_req && requested==1 && npins==2 && bias_calls==0);
    assert(requested_pins[0]==6 && requested_pins[1]==19);
    int64_t start=0;
    assert(xs8_gpio_send(&bus,0x09,&start)==0);
    assert(start>0 && reads==0 && sleeps==20 && writes==22);
    /* One settle sleep + 19 waveform sleeps; STOP has two long LOW bits. */
    assert(last_deadline-start==1150000+9800000+2*9800000+6*4900000+8*4900000+750000);
    assert(first_deadline<start && levels[6]==0 && levels[19]==0);
    levels[6]=levels[19]=1; fail_pin=6;
    assert(xs8_gpio_release(&bus)<0 && levels[19]==0); /* independent LOW attempts */
    fail_pin=0; xs8_gpio_close(&bus); assert(levels[6]==0 && !bus.tx_only);

    reset(); assert(xs8_gpio_open_tx_only(&bus)==0);
    fail_write=5; /* Fail an actual frame DATA write after BUSY went HIGH. */
    assert(xs8_gpio_send(&bus,0x09,&start)<0 && errno==EIO);
    assert(levels[6]==0 && levels[19]==0 && reads==0);
    xs8_gpio_close(&bus);

    for (unsigned int capture=0;capture<2;++capture) {
        reset(); assert(xs8_gpio_open(&bus,capture!=0)==0);
        assert(!bus.tx_only && bus.rx_req && requested==2 && npins==(capture?4u:3u));
        assert(requested_pins[2]==26 && (!capture || requested_pins[3]==13));
        assert(bias_calls==1);
        assert(xs8_gpio_send(&bus,0x09,&start)==0 && reads>0);
        busy_high=true; assert(xs8_gpio_send(&bus,0x09,&start)<0 && errno==ETIMEDOUT);
        xs8_gpio_close(&bus);
    }
    for (unsigned int capture=0;capture<2;++capture) {
        reset(); fail_bias=true;
        assert(xs8_gpio_open(&bus,capture!=0)<0 && errno==ENOTSUP);
        assert(bias_calls==1 && requested==0 && allocations==frees && chip_closes==1);
        assert(!bus.chip && !bus.tx_req && !bus.rx_req && !bus.lock_ready);
        reset(); fail_rx_request=true;
        assert(xs8_gpio_open(&bus,capture!=0)<0 && errno==EBUSY);
        assert(requested==1 && releases==1 && allocations==frees && chip_closes==1);
        assert(levels[6]==0 && levels[19]==0 && writes==2);
        assert(!bus.chip && !bus.tx_req && !bus.rx_req && !bus.lock_ready);
    }
    reset(); fail_bias=true; /* TX-only must not call the failing RX setter. */
    assert(xs8_gpio_open_tx_only(&bus)==0 && bias_calls==0 && !bus.rx_req);
    xs8_gpio_close(&bus);
    assert(allocations==frees && releases==1 && chip_closes==1);
    reset(); memset(&bus,0,sizeof(bus)); bus.tx_req=&requests[0];
    assert(xs8_gpio_send(&bus,0x09,&start)<0 && errno==EINVAL && writes==0);
    reset(); fail_request=true;
    assert(xs8_gpio_open_tx_only(&bus)<0 && !bus.tx_req && !bus.rx_req && !bus.chip);
    puts("TX-only GPIO ownership, production BUSY checks, timing and cleanup tests passed");
    return 0;
}
