#include <iostream>
#include <libcec/cec.h>
#include <libcec/cecloader.h>

#include "volume_control.h"
#include "xs8_gpio.h"
#include "xs8_monitor.h"

#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

using namespace CEC;
using Clock = VolumeControl::Clock;

static constexpr auto WATCHDOG = std::chrono::milliseconds(1200);
static constexpr auto DEFAULT_MAX_HOLD = std::chrono::milliseconds(2000);

struct KeyEvent {
    cec_user_control_code code;
    unsigned int duration;
    Clock::time_point received;
};

static volatile std::sig_atomic_t stopping = 0;
static std::mutex queueMutex;
static std::deque<KeyEvent> queue;

static void onSignal(int) { stopping = 1; }

static void onKey(void*, const cec_keypress* key) {
    if (!key) return;
    std::lock_guard<std::mutex> lock(queueMutex);
    queue.push_back({key->keycode, key->duration, Clock::now()});
}

class GpioTransmitter final : public VolumeTransmitter {
public:
    explicit GpioTransmitter(xs8_gpio& bus) : bus_(bus) {}
    bool send(uint8_t byte) override {
        int64_t start = 0;
        if (xs8_gpio_send(&bus_, byte, &start) == 0) return true;
        std::fprintf(stderr, "XS8 TX 0x%02X failed; delivery uncertain\n", byte);
        return false;
    }
private:
    xs8_gpio& bus_;
};

static bool parse_options(int argc, char** argv, bool& recovery,
                          bool& authorized, bool& monitor_xs8,
                          VolumeControl::Ms& max_hold) {
    recovery = false;
    authorized = false;
    monitor_xs8 = false;
    max_hold = DEFAULT_MAX_HOLD;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--startup-recovery") == 0) recovery = true;
        else if (std::strcmp(argv[i], "--allow-unrecovered-start") == 0)
            authorized = true;
        else if (std::strcmp(argv[i], "--monitor-xs8") == 0)
            monitor_xs8 = true;
        else if (std::strcmp(argv[i], "--max-hold-ms") == 0 && i + 1 < argc) {
            char* end = nullptr;
            const char* argument = argv[++i];
            errno = 0;
            const long value = std::strtol(argument, &end, 10);
            if (end == argument || *end || errno == ERANGE ||
                value < 100 || value > 3000) return false;
            max_hold = VolumeControl::Ms(value);
        } else return false;
    }
    return !(recovery && authorized);
}

int main(int argc, char** argv) {
    bool recovery = false, authorized = false, monitor_xs8 = false;
    VolumeControl::Ms max_hold{};
    if (!parse_options(argc, argv, recovery, authorized, monitor_xs8,
                       max_hold)) {
        std::fprintf(stderr, "Usage: %s [--startup-recovery | "
                             "--allow-unrecovered-start] "
                             "[--max-hold-ms 100..3000] [--monitor-xs8]\n",
                     argv[0]);
        return 2;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    xs8_gpio bus{};
    if (xs8_gpio_open(&bus, monitor_xs8) < 0) {
        std::perror("Cannot open XS8 GPIO");
        std::fprintf(stderr, "WARNING: receiver volume state is unknown; "
                             "check it at the receiver\n");
        return 1;
    }
    struct xs8_monitor *monitor = nullptr;
    if (monitor_xs8 && xs8_monitor_start(&bus, &monitor) < 0) {
        std::perror("Cannot start XS8 bus monitor");
        xs8_gpio_close(&bus);
        return 1;
    }
    GpioTransmitter tx(bus);
    VolumeControl volume(tx, WATCHDOG, max_hold, [] { return stopping != 0; });
    if (!volume.startup(recovery, authorized) && recovery) {
        std::fprintf(stderr, "FAULT: startup recovery failed; "
                             "receiver volume may remain active\n");
        volume.shutdown(Clock::now());
        xs8_monitor_stop(monitor);
        xs8_gpio_close(&bus);
        return 1;
    }
    if (volume.state() == VolumeControl::State::BOOT_INHIBIT)
        std::fprintf(stderr, "BOOT_INHIBIT: GPIO ready, CEC controls disabled; "
                             "startup recovery is unvalidated\n");

    libcec_configuration config;
    config.Clear();
    config.bActivateSource = 0;
    config.wakeDevices.Clear();
    config.clientVersion = LIBCEC_VERSION_CURRENT;
    config.deviceTypes.Add(CEC_DEVICE_TYPE_AUDIO_SYSTEM);
    std::snprintf(config.strDeviceName, sizeof(config.strDeviceName),
                  "Kenwood Bridge");
    ICECCallbacks callbacks{};
    callbacks.keyPress = onKey;
    config.callbacks = &callbacks;

    ICECAdapter* cec = LibCecInitialise(&config);
    if (!cec) {
        std::fprintf(stderr, "Cannot initialize libCEC\n");
        volume.shutdown(Clock::now());
        xs8_monitor_stop(monitor);
        xs8_gpio_close(&bus);
        return 1;
    }
    cec_adapter_descriptor adapters[10]{};
    const int count = cec->DetectAdapters(adapters, 10, nullptr, true);
    if (count <= 0 || !cec->Open(adapters[0].strComName)) {
        std::fprintf(stderr, "Cannot open CEC adapter\n");
        volume.shutdown(Clock::now());
        xs8_monitor_stop(monitor);
        UnloadLibCec(cec);
        xs8_gpio_close(&bus);
        return 1;
    }
    cec->SetInactiveView();
    std::puts("CEC Audio System ready");

    bool fault_reported = false;
    auto report_fault = [&] {
        if (volume.state() == VolumeControl::State::FAULT_LATCHED &&
            !fault_reported) {
            std::fprintf(stderr, "FAULT_LATCHED: XS8 state uncertain; "
                                 "CEC commands disabled. Check receiver.\n");
            fault_reported = true;
        }
    };
    while (!stopping) {
        volume.tick(Clock::now());
        report_fault();
        std::deque<KeyEvent> pending;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            pending.swap(queue);
        }
        for (const auto& event : pending) {
            if (stopping) break;
            const auto now = Clock::now();
            volume.tick(now); // A busy event queue must not postpone STOP.
            const bool released = event.duration > 0;
            if (event.code == CEC_USER_CONTROL_CODE_VOLUME_UP ||
                event.code == CEC_USER_CONTROL_CODE_VOLUME_DOWN) {
                auto direction = event.code == CEC_USER_CONTROL_CODE_VOLUME_UP
                    ? VolumeControl::Direction::UP : VolumeControl::Direction::DOWN;
                const auto processed = Clock::now();
                const auto mono_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    processed.time_since_epoch()).count();
                const auto age_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    processed - event.received).count();
                if (released) volume.release(direction, now);
                else volume.press(direction, event.received, now);
                std::fprintf(stderr, "mono_us=%lld CEC volume=%s direction=%s "
                                     "duration_ms=%u event_age_us=%lld\n",
                             static_cast<long long>(mono_us),
                             released ? "RELEASE" : "PRESS",
                             direction == VolumeControl::Direction::UP ? "UP" : "DOWN",
                             static_cast<unsigned int>(event.duration),
                             static_cast<long long>(age_us));
            } else if (event.code == CEC_USER_CONTROL_CODE_MUTE && !released &&
                       event.received <= now && now - event.received < WATCHDOG) {
                volume.mute(now);
            }
            report_fault();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    bool stopped = volume.shutdown(Clock::now());
    if (!stopped || volume.stop_owed() || volume.context_may_be_open())
        std::fprintf(stderr, "WARNING: XS8 cleanup incomplete; "
                             "receiver volume may remain active\n");
    cec->Close();
    UnloadLibCec(cec);
    xs8_monitor_stop(monitor);
    const bool released = xs8_gpio_release(&bus) == 0;
    if (!released) std::fprintf(stderr, "WARNING: GPIO TX release failed\n");
    xs8_gpio_close(&bus);
    return stopped && released ? 0 : 1;
}
