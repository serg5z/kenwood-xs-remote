#include "xs8_decode.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

struct fixture {
    struct xs8_decoder decoder;
    struct xs8_decoded results[16];
    size_t count;
    uint64_t time_ns;
    uint64_t sequence;
};

static void collect(void *user, const struct xs8_decoded *result)
{
    struct fixture *fixture = user;
    assert(fixture->count < 16);
    fixture->results[fixture->count++] = *result;
}

static void edge(struct fixture *fixture, unsigned int gpio,
                 enum xs8_edge_kind kind, uint64_t after_ns)
{
    fixture->time_ns += after_ns;
    struct xs8_edge event = {
        .time_ns = fixture->time_ns,
        .sequence = ++fixture->sequence,
        .gpio = gpio,
        .kind = kind
    };
    xs8_decoder_edge(&fixture->decoder, &event, collect, fixture);
}

static void begin(struct fixture *fixture)
{
    edge(fixture, 23, XS8_RISING, 1000000);
    edge(fixture, 27, XS8_RISING, 1150000);
    edge(fixture, 27, XS8_FALLING, 9800000);
}

static void bit(struct fixture *fixture, bool one, bool last)
{
    edge(fixture, 27, XS8_RISING, one ? 9800000 : 4900000);
    edge(fixture, 27, XS8_FALLING, 4900000);
    if (last) edge(fixture, 23, XS8_FALLING, 750000);
}

static void frame(struct fixture *fixture, uint8_t value)
{
    begin(fixture);
    for (unsigned int i = 0; i < 8; ++i)
        bit(fixture, (value & (1u << i)) != 0, i == 7);
}

int main(void)
{
    struct fixture fixture = {0};
    xs8_decoder_init(&fixture.decoder, true);
    frame(&fixture, 0x09);
    assert(fixture.count == 1);
    assert(fixture.results[0].valid);
    assert(fixture.results[0].waveform_lsb == 0x09);
    assert(fixture.results[0].legacy_msb_short1 == 0x6F);
    assert(fixture.results[0].time_ns == 1000000);

    frame(&fixture, 0x41);
    assert(fixture.count == 2);
    assert(fixture.results[1].valid);
    assert(fixture.results[1].legacy_msb_short1 == 0x7D);

    begin(&fixture);
    for (unsigned int i = 0; i < 3; ++i) bit(&fixture, false, false);
    edge(&fixture, 23, XS8_FALLING, 750000);
    assert(fixture.count == 3);
    assert(!fixture.results[2].valid && fixture.results[2].bits == 3);

    begin(&fixture);
    bit(&fixture, true, false);
    ++fixture.sequence; /* A kernel global sequence-number gap. */
    bit(&fixture, false, false);
    edge(&fixture, 23, XS8_FALLING, 750000);
    assert(fixture.count == 5);
    assert(!fixture.results[3].valid);
    assert(strcmp(fixture.results[3].reason, "EVENT_GAP") == 0);
    assert(fixture.results[4].kind == XS8_EVENT_GAP);
    assert(fixture.results[4].lost_events == 1);
    frame(&fixture, 0xE9);
    assert(fixture.count == 6 && fixture.results[5].valid);
    assert(fixture.results[5].waveform_lsb == 0xE9);

    struct fixture initial_gap = {0};
    xs8_decoder_init(&initial_gap.decoder, true);
    initial_gap.sequence = 1; /* First delivered event has sequence 2. */
    frame(&initial_gap, 0x09);
    assert(initial_gap.count == 1);
    assert(initial_gap.results[0].kind == XS8_EVENT_GAP);
    assert(initial_gap.results[0].lost_events == 1);

    struct fixture bad_tail = {0};
    xs8_decoder_init(&bad_tail.decoder, true);
    begin(&bad_tail);
    for (unsigned int i = 0; i < 7; ++i) bit(&bad_tail, false, false);
    edge(&bad_tail, 27, XS8_RISING, 4900000);
    edge(&bad_tail, 27, XS8_FALLING, 1000000);
    edge(&bad_tail, 23, XS8_FALLING, 750000);
    assert(bad_tail.count == 1 && !bad_tail.results[0].valid);
    assert(strcmp(bad_tail.results[0].reason, "TAIL_TIMING") == 0);

    begin(&fixture);
    xs8_decoder_flush(&fixture.decoder, collect, &fixture);
    assert(fixture.count == 7 && !fixture.results[6].valid);
    puts("XS8 offline decoder tests passed");
    return 0;
}
