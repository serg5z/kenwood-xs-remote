#ifndef XS8_GPIO_H
#define XS8_GPIO_H

#include <gpiod.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xs8_gpio {
    struct gpiod_chip *chip;
    struct gpiod_line_request *tx_req;
    struct gpiod_line_request *rx_req;
    pthread_mutex_t rx_lock;
    bool lock_ready;
    int rt_priority;
    bool tx_only; /* Explicit disconnected-fixture initialization only. */
};

/* One owner sends frames. The RX lock also protects a concurrent capture reader. */
int xs8_gpio_open(struct xs8_gpio *bus, bool capture);
/* Diagnostic API: requests TX only; no BUSY sensing. Never use on a live bus. */
int xs8_gpio_open_tx_only(struct xs8_gpio *bus);
int xs8_gpio_send(struct xs8_gpio *bus, uint8_t value, int64_t *tx_start_ns);
int xs8_gpio_release(struct xs8_gpio *bus);
void xs8_gpio_close(struct xs8_gpio *bus);
int64_t xs8_gpio_now_ns(void);
int xs8_gpio_wait_until(int64_t deadline_ns);

#ifdef __cplusplus
}
#endif

#endif
