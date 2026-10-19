#include "xs8_decode.h"
#include <string.h>

#define BUSY_GPIO 26
#define DATA_GPIO 13
#define SHORT_LONG_SPLIT_NS 7350000ULL

static bool within(uint64_t duration, uint64_t low, uint64_t high)
{
    return duration >= low && duration <= high;
}

static void emit_frame(struct xs8_decoder *decoder, const char *reason,
                       xs8_decode_output output, void *user)
{
    if (!decoder->active) return;
    struct xs8_decoded result = {
        .kind = XS8_FRAME,
        .time_ns = decoder->frame_start_ns,
        .valid = reason == NULL && decoder->error == NULL &&
                 decoder->bits == 8 && decoder->stage == 4,
        .waveform_lsb = decoder->waveform_lsb,
        .legacy_msb_short1 = decoder->legacy_msb_short1,
        .bits = decoder->bits,
        .reason = reason ? reason : decoder->error,
        .final_data_rise_ns = decoder->final_data_rise_ns,
        .final_data_fall_ns = decoder->final_data_fall_ns,
        .busy_fall_ns = decoder->busy_fall_ns,
        .tail_fell = decoder->tail_fell,
        .global_gap_warnings = decoder->global_gap_warnings
    };
    if (!result.valid && !result.reason) result.reason = "TRUNCATED";
    output(user, &result);
    decoder->active = false;
}

void xs8_decoder_init(struct xs8_decoder *decoder, bool busy_low)
{
    memset(decoder, 0, sizeof(*decoder));
    decoder->synced = busy_low;
}

void xs8_decoder_edge(struct xs8_decoder *decoder, const struct xs8_edge *edge,
                      xs8_decode_output output, void *user)
{
    if (edge->sequence != decoder->last_sequence + 1) {
        uint64_t lost = edge->sequence > decoder->last_sequence + 1
            ? edge->sequence - decoder->last_sequence - 1 : 0;
        emit_frame(decoder, "EVENT_GAP", output, user);
        struct xs8_decoded gap = {
            .kind = XS8_EVENT_GAP, .time_ns = edge->time_ns,
            .lost_events = lost, .reason = lost ? "MISSING_EVENTS" : "SEQUENCE_RESET"
        };
        output(user, &gap);
        decoder->synced = false;
    }
    decoder->last_sequence = edge->sequence;

    if (edge->gpio == BUSY_GPIO) {
        if (edge->kind == XS8_FALLING) {
            decoder->busy_fall_ns = edge->time_ns;
            if (decoder->active && decoder->stage == 4 &&
                !decoder->tail_fell &&
                (edge->time_ns < decoder->phase_start_ns ||
                 edge->time_ns - decoder->phase_start_ns < 3000000))
                decoder->error = "TAIL_TIMING";
            emit_frame(decoder, NULL, output, user);
            decoder->synced = true;
        } else if (decoder->synced) {
            emit_frame(decoder, "NEW_BUSY_RISE", output, user);
            decoder->active = true;
            decoder->frame_start_ns = edge->time_ns;
            decoder->phase_start_ns = edge->time_ns;
            decoder->waveform_lsb = 0;
            decoder->legacy_msb_short1 = 0;
            decoder->bits = 0;
            decoder->stage = 0;
            decoder->tail_fell = false;
            decoder->global_gap_warnings = 0;
            decoder->final_data_rise_ns = 0;
            decoder->final_data_fall_ns = 0;
            decoder->busy_fall_ns = 0;
            decoder->error = NULL;
        }
        return;
    }
    if (edge->gpio != DATA_GPIO || !decoder->active) return;
    if (edge->time_ns < decoder->phase_start_ns) {
        decoder->error = "NONMONOTONIC_EDGE";
        return;
    }
    uint64_t elapsed = edge->time_ns - decoder->phase_start_ns;
    if (edge->kind == XS8_RISING) {
        if (decoder->stage == 0) {
            if (!within(elapsed, 300000, 5000000)) decoder->error = "LEAD_TIMING";
            decoder->stage = 1;
        } else if (decoder->stage == 2) {
            if (!within(elapsed, 3000000, 12000000))
                decoder->error = "LOW_TIMING";
            bool long_low = elapsed >= SHORT_LONG_SPLIT_NS;
            if (long_low) decoder->waveform_lsb |= (uint8_t)(1u << decoder->bits);
            decoder->legacy_msb_short1 = (uint8_t)(
                (decoder->legacy_msb_short1 << 1) | !long_low);
            ++decoder->bits;
            decoder->stage = decoder->bits == 8 ? 4 : 3;
            if (decoder->bits == 8) decoder->final_data_rise_ns = edge->time_ns;
        } else decoder->error = "UNEXPECTED_DATA_EDGE";
    } else {
        if (decoder->stage == 1) {
            if (!within(elapsed, 7000000, 13000000))
                decoder->error = "START_TIMING";
            decoder->stage = 2;
        } else if (decoder->stage == 3) {
            if (!within(elapsed, 3000000, 7000000))
                decoder->error = "HIGH_TIMING";
            decoder->stage = 2;
        } else if (decoder->stage == 4) {
            if (decoder->tail_fell || !within(elapsed, 3000000, 7000000))
                decoder->error = "TAIL_TIMING";
            decoder->tail_fell = true;
            decoder->final_data_fall_ns = edge->time_ns;
        } else decoder->error = "UNEXPECTED_DATA_EDGE";
    }
    decoder->phase_start_ns = edge->time_ns;
}

void xs8_decoder_flush(struct xs8_decoder *decoder, xs8_decode_output output,
                       void *user)
{
    emit_frame(decoder, "CAPTURE_ENDED", output, user);
}

bool xs8_decoded_local_tx_tail_valid(const struct xs8_decoded *frame)
{
    if (frame->kind != XS8_FRAME || !frame->valid || !frame->tail_fell ||
        frame->final_data_fall_ns <= frame->final_data_rise_ns ||
        frame->busy_fall_ns <= frame->final_data_fall_ns) return false;
    return within(frame->final_data_fall_ns - frame->final_data_rise_ns,
                  3000000, 7000000) &&
           frame->busy_fall_ns - frame->final_data_fall_ns <= 2000000;
}
