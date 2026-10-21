/* Actual sniffer setup/cleanup with fake libgpiod; no GPIO or input events. */
#define main xs8_sniff_main
#include "../xs8_sniff.c"
#undef main
#include <assert.h>
#include <unistd.h>

struct gpiod_chip { int unused; };
struct gpiod_line_settings { bool input; enum gpiod_line_bias bias; };
struct gpiod_line_config { unsigned int pins[2]; size_t count; enum gpiod_line_bias bias; };
struct gpiod_request_config { int unused; };
struct gpiod_line_request { int pipe[2]; };
struct gpiod_edge_event_buffer { int unused; };
static struct gpiod_chip chip;
enum failure { NONE, DIRECTION, BIAS, EDGE, CLOCK, ADD, REQUEST };
static enum failure fail_at;
static unsigned int allocations, frees, chip_closes, request_attempts, releases, bias_calls;
static void *allocate(size_t size) { void *p=calloc(1,size); assert(p); ++allocations; return p; }
static void dispose(void *p) { assert(p); ++frees; free(p); }
static int setting_result(enum failure stage)
{ if(fail_at==stage){errno=ENOTSUP;return -1;} return 0; }

struct gpiod_chip *gpiod_chip_open(const char *path)
{ assert(!strcmp(path,"/dev/gpiochip0")); return &chip; }
void gpiod_chip_close(struct gpiod_chip *c) { assert(c==&chip); ++chip_closes; }
struct gpiod_line_settings *gpiod_line_settings_new(void)
{
    struct gpiod_line_settings *s=allocate(sizeof(*s));
    s->bias=GPIOD_LINE_BIAS_AS_IS; return s;
}
void gpiod_line_settings_free(struct gpiod_line_settings *s) { dispose(s); }
int gpiod_line_settings_set_direction(struct gpiod_line_settings *s, enum gpiod_line_direction d)
{ assert(d==GPIOD_LINE_DIRECTION_INPUT); s->input=true; return setting_result(DIRECTION); }
int gpiod_line_settings_set_bias(struct gpiod_line_settings *s, enum gpiod_line_bias b)
{
    assert(s->input && b==GPIOD_LINE_BIAS_DISABLED); ++bias_calls;
    if(setting_result(BIAS)<0)return -1;
    s->bias=b; return 0;
}
int gpiod_line_settings_set_edge_detection(struct gpiod_line_settings *s, enum gpiod_line_edge e)
{ assert(s->input && e==GPIOD_LINE_EDGE_BOTH); return setting_result(EDGE); }
int gpiod_line_settings_set_event_clock(struct gpiod_line_settings *s, enum gpiod_line_clock c)
{ assert(s->input && c==GPIOD_LINE_CLOCK_MONOTONIC); return setting_result(CLOCK); }
struct gpiod_line_config *gpiod_line_config_new(void) { return allocate(sizeof(struct gpiod_line_config)); }
void gpiod_line_config_free(struct gpiod_line_config *c) { dispose(c); }
int gpiod_line_config_add_line_settings(struct gpiod_line_config *c,
    const unsigned int *pins, size_t n, struct gpiod_line_settings *s)
{
    assert(n==2 && pins[0]==26 && pins[1]==13 && s->input);
    assert(s->bias==GPIOD_LINE_BIAS_DISABLED);
    c->count=n; memcpy(c->pins,pins,n*sizeof(*pins)); c->bias=s->bias;
    return setting_result(ADD);
}
struct gpiod_request_config *gpiod_request_config_new(void) { return allocate(sizeof(struct gpiod_request_config)); }
void gpiod_request_config_free(struct gpiod_request_config *c) { dispose(c); }
void gpiod_request_config_set_event_buffer_size(struct gpiod_request_config *c, size_t n)
{ assert(c && n==4096); }
void gpiod_request_config_set_consumer(struct gpiod_request_config *c, const char *name)
{ assert(c && !strcmp(name,"xs8-sniff")); }
struct gpiod_line_request *gpiod_chip_request_lines(struct gpiod_chip *c,
    struct gpiod_request_config *r, struct gpiod_line_config *cfg)
{
    assert(c==&chip && r && cfg->count==2 && cfg->bias==GPIOD_LINE_BIAS_DISABLED);
    ++request_attempts;
    if(setting_result(REQUEST)<0)return NULL;
    struct gpiod_line_request *req=allocate(sizeof(*req));
    assert(pipe(req->pipe)==0);
    stop_requested=1; /* End capture immediately, never read or wait on hardware. */
    return req;
}
void gpiod_line_request_release(struct gpiod_line_request *r)
{ ++releases; close(r->pipe[0]); close(r->pipe[1]); dispose(r); }
int gpiod_line_request_get_fd(struct gpiod_line_request *r) { return r->pipe[0]; }
struct gpiod_edge_event_buffer *gpiod_edge_event_buffer_new(size_t n)
{ assert(n==256); return allocate(sizeof(struct gpiod_edge_event_buffer)); }
void gpiod_edge_event_buffer_free(struct gpiod_edge_event_buffer *b) { dispose(b); }
int gpiod_line_request_read_edge_events(struct gpiod_line_request *r,
    struct gpiod_edge_event_buffer *b, size_t n)
{ (void)r; (void)b; (void)n; assert(!"Unexpected edge read"); return -1; }
struct gpiod_edge_event *gpiod_edge_event_buffer_get_event(struct gpiod_edge_event_buffer *b, unsigned long i)
{ (void)b; (void)i; abort(); }
uint64_t gpiod_edge_event_get_timestamp_ns(struct gpiod_edge_event *e) { (void)e; abort(); }
unsigned long gpiod_edge_event_get_global_seqno(struct gpiod_edge_event *e) { (void)e; abort(); }
unsigned long gpiod_edge_event_get_line_seqno(struct gpiod_edge_event *e) { (void)e; abort(); }
unsigned int gpiod_edge_event_get_line_offset(struct gpiod_edge_event *e) { (void)e; abort(); }
enum gpiod_edge_event_type gpiod_edge_event_get_event_type(struct gpiod_edge_event *e)
{ (void)e; abort(); }

int main(void)
{
    for(unsigned int diagnostic=0; diagnostic<2; ++diagnostic) {
        for(enum failure f=NONE; f<=REQUEST; ++f) {
            allocations=frees=chip_closes=request_attempts=releases=bias_calls=0;
            stop_requested=0; fail_at=f;
            char *args[]={"xs8-sniff-test", "--event-order", NULL};
            int result=xs8_sniff_main(diagnostic?2:1,args);
            assert(result==(f==NONE?0:1));
            assert(allocations==frees && chip_closes==1);
            assert(bias_calls==(f==DIRECTION?0u:1u));
            assert(request_attempts==((f==NONE || f==REQUEST)?1u:0u));
            assert(releases==(f==NONE?1u:0u));
        }
    }
    puts("Sniffer RX bias, configuration failures and cleanup tests passed");
    return 0;
}
