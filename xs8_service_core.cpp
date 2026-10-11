#include "xs8_service_core.h"

#include <chrono>
#include <cstdio>

namespace {
const char *state_name(VolumeControl::State state)
{
    using State = VolumeControl::State;
    switch (state) {
    case State::BOOT_INHIBIT: return "BOOT_INHIBIT";
    case State::IDLE: return "IDLE";
    case State::ENTERING: return "ENTERING";
    case State::START_UNCERTAIN: return "START_UNCERTAIN";
    case State::HOLDING_UP: return "HOLDING_UP";
    case State::HOLDING_DOWN: return "HOLDING_DOWN";
    case State::STOP_PENDING: return "STOP_PENDING";
    case State::EXIT_PENDING: return "EXIT_PENDING";
    case State::FAULT_LATCHED: return "FAULT_LATCHED";
    }
    return "UNKNOWN";
}

const char *waveform_name(WaveformEvidence evidence)
{
    switch (evidence) {
    case WaveformEvidence::NOT_OBSERVED: return "NOT_OBSERVED";
    case WaveformEvidence::MATCH: return "MATCH";
    case WaveformEvidence::MISMATCH: return "MISMATCH";
    case WaveformEvidence::INVALID: return "INVALID";
    case WaveformEvidence::MISSING: return "MISSING";
    }
    return "UNKNOWN";
}

const char *receiver_name(ReceiverEvidence)
{
    return "UNVERIFIED";
}
}

Xs8ServiceCore::Xs8ServiceCore(ServiceTransmitter& tx,
    VolumeControl::Ms max_hold, bool operator_authorized,
    std::function<bool()> stopping, std::function<Time()> now)
    : tx_(tx), volume_(tx, VolumeControl::Ms(1200), max_hold,
                       std::move(stopping), std::move(now))
{
    // This sends no command. No automatic recovery is available in phase 1.
    volume_.startup(false, operator_authorized);
}

void Xs8ServiceCore::connected() { client_connected_ = true; }

std::string Xs8ServiceCore::status() const
{
    ServiceTxEvidence evidence = tx_.last_evidence();
    ServiceTxEvidence stop = tx_.last_stop_evidence();
    char last_byte[8];
    if (evidence.attempted)
        std::snprintf(last_byte, sizeof(last_byte), "0x%02X", evidence.byte);
    else std::snprintf(last_byte, sizeof(last_byte), "NONE");
    return std::string("OK state=") + state_name(volume_.state()) +
        " stop_owed=" + (volume_.stop_owed() ? "1" : "0") +
        " context_may_be_open=" +
        (volume_.context_may_be_open() ? "1" : "0") +
        " monitor=" + (tx_.monitor_healthy() ? "HEALTHY" : "FAILED") +
        " last_byte=" + last_byte +
        " last_gpio_tx=" + (evidence.attempted
            ? evidence.gpio_completed ? "TX_OK" : "TX_FAIL" : "NONE") +
        " last_waveform=" + waveform_name(evidence.waveform) +
        " last_stop_waveform=" + waveform_name(stop.waveform) +
        " receiver_execution=" + receiver_name(evidence.receiver) + "\n";
}

std::string Xs8ServiceCore::command(const std::string& line, Time received,
                                    Time now)
{
    tick(now);
    if (line == "STATUS") return status();
    if (!client_connected_) return "ERR NO_CLIENT\n";
    if (!tx_.monitor_healthy()) return "ERR MONITOR_FAILED\n";
    if (line == "MUTE")
        return volume_.mute(now) ? "OK MUTE_TX\n" : "ERR MUTE_UNCERTAIN\n";
    const bool press = line == "VOLUME_PRESS UP" ||
                       line == "VOLUME_PRESS DOWN";
    const bool release = line == "VOLUME_RELEASE UP" ||
                         line == "VOLUME_RELEASE DOWN";
    if (!press && !release) return "ERR UNKNOWN_COMMAND\n";
    VolumeControl::Direction direction = line.size() >= 2 &&
        line.compare(line.size() - 2, 2, "UP") == 0
        ? VolumeControl::Direction::UP : VolumeControl::Direction::DOWN;
    if (press) {
        if (!volume_.press(direction, received, now))
            return std::string("ERR ") + state_name(volume_.state()) + "\n";
        owner_direction_ = direction;
        return "OK VOLUME_PRESS_TX\n";
    }
    return volume_.release(direction, now) ? "OK VOLUME_RELEASE_TX\n"
                                           : "ERR VOLUME_STOP_UNCERTAIN\n";
}

void Xs8ServiceCore::disconnected(Time now)
{
    if (!client_connected_) return;
    client_connected_ = false;
    if (volume_.state() == VolumeControl::State::HOLDING_UP ||
        volume_.state() == VolumeControl::State::HOLDING_DOWN)
        volume_.disconnect(owner_direction_, now);
    else if (volume_.stop_owed() || volume_.context_may_be_open())
        volume_.shutdown(now);
}

void Xs8ServiceCore::tick(Time now)
{
    if (!tx_.monitor_healthy()) {
        if (!monitor_fault_handled_ &&
            (volume_.stop_owed() || volume_.context_may_be_open())) {
            monitor_fault_handled_ = true;
            volume_.shutdown(now);
        }
        return;
    }
    volume_.tick(now);
}

bool Xs8ServiceCore::shutdown(Time now) { return volume_.shutdown(now); }
