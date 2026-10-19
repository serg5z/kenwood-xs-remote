#ifndef XS8_DECODE_H
#define XS8_DECODE_H

#include <stdbool.h>
#include <stdint.h>

enum xs8_edge_kind { XS8_RISING, XS8_FALLING };
enum xs8_decode_kind { XS8_FRAME, XS8_EVENT_GAP };

struct xs8_edge {
    uint64_t time_ns;
    uint64_t sequence;
    unsigned int gpio;
    enum xs8_edge_kind kind;
};

struct xs8_decoded {
    enum xs8_decode_kind kind;
    uint64_t time_ns;
    uint64_t lost_events;
    bool valid;
    uint8_t waveform_lsb;
    uint8_t legacy_msb_short1;
    unsigned int bits;
    const char *reason;
    /* Observations at frame emission, not receiver acknowledgments. A passive
     * BUSY-first frame can be valid before its final DATA fall is observed. */
    uint64_t final_data_rise_ns, final_data_fall_ns, busy_fall_ns;
    bool tail_fell;
    uint64_t global_gap_warnings; /* exempt gaps spanning this frame */
};

typedef void (*xs8_decode_output)(void *, const struct xs8_decoded *);

struct xs8_decoder {
    uint64_t last_sequence;
    uint64_t frame_start_ns;
    uint64_t phase_start_ns;
    uint8_t waveform_lsb;
    uint8_t legacy_msb_short1;
    unsigned int bits;
    unsigned int stage;
    bool synced;
    bool active;
    bool tail_fell;
    uint64_t global_gap_warnings;
    uint64_t final_data_rise_ns, final_data_fall_ns, busy_fall_ns;
    const char *error;
};

void xs8_decoder_init(struct xs8_decoder *decoder, bool busy_low);
void xs8_decoder_edge(struct xs8_decoder *decoder, const struct xs8_edge *edge,
                      xs8_decode_output output, void *user);
void xs8_decoder_flush(struct xs8_decoder *decoder, xs8_decode_output output,
                       void *user);
/* Stricter than passive validity: our GPIO TX ends DATA HIGH (~4.9 ms),
 * then drops DATA before BUSY (~0.75 ms guard). Allows a 3..7 ms HIGH
 * and a strictly positive guard up to 2 ms; does not prove execution. */
bool xs8_decoded_local_tx_tail_valid(const struct xs8_decoded *frame);

#endif
