#include "xs8_decode.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define XS8_SEND_VERIFY_TEST
#include "../xs8_send.c"

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

struct capture_fixture {
    struct capture capture;
    struct captured_edge edges[128];
    uint64_t time_ns, sequence;
};

static void captured_edge(struct capture_fixture *fixture, unsigned int gpio,
                          enum xs8_edge_kind kind, uint64_t after_ns)
{
    fixture->time_ns += after_ns;
    assert(fixture->capture.count < 128);
    fixture->edges[fixture->capture.count++] = (struct captured_edge){
        fixture->time_ns, ++fixture->sequence, gpio, kind
    };
}

static void captured_frame(struct capture_fixture *fixture, uint8_t value)
{
    captured_edge(fixture, 23, XS8_RISING, 1000000);
    captured_edge(fixture, 27, XS8_RISING, 1150000);
    captured_edge(fixture, 27, XS8_FALLING, 9800000);
    for (unsigned int i = 0; i < 8; ++i) {
        captured_edge(fixture, 27, XS8_RISING,
                      (value & (1u << i)) ? 9800000 : 4900000);
        captured_edge(fixture, 27, XS8_FALLING, 4900000);
    }
    captured_edge(fixture, 23, XS8_FALLING, 750000);
}

static struct capture_fixture new_capture(void)
{
    struct capture_fixture fixture = {0};
    fixture.capture.started = true;
    fixture.capture.initial_busy_low = true;
    fixture.capture.end_ns = 1000000000;
    return fixture;
}

static void test_self_capture(void)
{
    struct capture_fixture fixture = new_capture();
    captured_frame(&fixture, 0x09);
    fixture.capture.edges = fixture.edges;
    struct self_capture_result result = verify_self_capture(
        &fixture.capture, 0x09, true, 500000, 150000000, false);
    assert(result.capture == CAPTURE_COMPLETE && result.match == MATCH);
    assert(result.observed_lsb == 0x09 && result.observed_legacy == 0x6F);

    result = verify_self_capture(&fixture.capture, 0xE9, true,
                                 500000, 150000000, false);
    assert(result.capture == CAPTURE_COMPLETE && result.match == MISMATCH);

    result = verify_self_capture(&fixture.capture, 0x09, false,
                                 500000, 150000000, false);
    assert(result.capture == CAPTURE_COMPLETE && result.match == MATCH_UNKNOWN);

    struct capture_fixture missing = new_capture();
    captured_frame(&missing, 0x09);
    missing.capture.edges = missing.edges;
    ++missing.edges[5].sequence;
    for (size_t i = 6; i < missing.capture.count; ++i)
        ++missing.edges[i].sequence;
    missing.capture.lost = 1;
    result = verify_self_capture(&missing.capture, 0x09, true,
                                 500000, 150000000, false);
    assert(result.capture == CAPTURE_INVALID && result.match == MATCH_UNKNOWN);

    struct capture_fixture truncated = new_capture();
    captured_edge(&truncated, 23, XS8_RISING, 1000000);
    captured_edge(&truncated, 27, XS8_RISING, 1150000);
    captured_edge(&truncated, 27, XS8_FALLING, 9800000);
    captured_edge(&truncated, 27, XS8_RISING, 4900000);
    truncated.capture.edges = truncated.edges;
    result = verify_self_capture(&truncated.capture, 0x09, true,
                                 500000, 150000000, false);
    assert(result.capture == CAPTURE_INCOMPLETE && result.match == MATCH_UNKNOWN);

    captured_frame(&fixture, 0xE9);
    result = verify_self_capture(&fixture.capture, 0x09, true,
                                 500000, 150000000, false);
    assert(result.capture == CAPTURE_COMPLETE && result.match == MATCH);
    assert(result.additional_frames == 1);

    /* The observed ROOT 0x13 exchange has two immediate follow-on frames.
     * The first follow-on may begin before the TX function returns. */
    struct capture_fixture response = new_capture();
    captured_frame(&response, 0x13);
    uint64_t tx_frame_end_ns = response.time_ns;
    captured_frame(&response, 0x37);
    captured_frame(&response, 0xE7);
    response.capture.edges = response.edges;
    result = verify_self_capture(&response.capture, 0x13, true,
                                 500000, tx_frame_end_ns + 2000000, false);
    assert(result.capture == CAPTURE_COMPLETE && result.match == MATCH);
    assert(result.observed_lsb == 0x13 && result.additional_frames == 2);

    result = verify_self_capture(&response.capture, 0x14, true,
                                 500000, tx_frame_end_ns + 2000000, false);
    assert(result.capture == CAPTURE_COMPLETE && result.match == MISMATCH);
    assert(result.additional_frames == 2);

    struct capture_fixture overlapping = new_capture();
    captured_frame(&overlapping, 0x13);
    overlapping.capture.edges = overlapping.edges;
    result = verify_self_capture(&overlapping.capture, 0x13, true,
                                 20000000, 150000000, false);
    assert(result.capture == CAPTURE_INVALID && result.match == MATCH_UNKNOWN);

    struct capture_fixture incomplete_response = new_capture();
    captured_frame(&incomplete_response, 0x13);
    captured_edge(&incomplete_response, 23, XS8_RISING, 1000000);
    captured_edge(&incomplete_response, 27, XS8_RISING, 1150000);
    incomplete_response.capture.edges = incomplete_response.edges;
    result = verify_self_capture(&incomplete_response.capture, 0x13, true,
                                 500000, 150000000, false);
    assert(result.capture == CAPTURE_INCOMPLETE && result.match == MATCH_UNKNOWN);
    assert(result.additional_frames == 1);

    struct capture_fixture later = new_capture();
    captured_frame(&later, 0x09);
    later.time_ns += 100000000; /* A separate frame after TX completed. */
    captured_frame(&later, 0xE9);
    later.capture.edges = later.edges;
    result = verify_self_capture(&later.capture, 0x09, true,
                                 500000, 150000000, false);
    assert(result.capture == CAPTURE_COMPLETE && result.match == MATCH);
    assert(result.additional_frames == 1);
}

static void test_self_capture_rejects_incompatible_tail(void)
{
    for (unsigned int variant = 0; variant < 4; ++variant) {
        struct capture_fixture fixture = new_capture();
        captured_frame(&fixture, 0x84);
        if (variant == 0) {
            /* Missing final DATA fall, dense global numbering. */
            fixture.edges[18] = fixture.edges[19];
            fixture.edges[18].sequence = 19;
            --fixture.capture.count;
        } else if (variant == 1) {
            /* BUSY-first: same bit value, different tail ordering. */
            fixture.edges[18].gpio = 23;
            fixture.edges[19].gpio = 27;
        } else if (variant == 2) {
            fixture.edges[19].timestamp_ns = fixture.edges[18].timestamp_ns;
        } else {
            fixture.edges[19].timestamp_ns = fixture.edges[18].timestamp_ns + 2000001;
        }
        fixture.capture.edges = fixture.edges;
        struct self_capture_result result = verify_self_capture(
            &fixture.capture, 0x84, true, 500000, 150000000, false);
        assert(result.capture == CAPTURE_INCOMPLETE && result.match == MATCH_UNKNOWN);
    }
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
    test_self_capture();
    test_self_capture_rejects_incompatible_tail();
    puts("XS8 offline decoder tests passed");
    return 0;
}
