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
        " receiver_execution=" + receiver_name(evidence.receiver) +
        receiver_status_ + "\n";
}

std::string Xs8ServiceCore::command(const std::string& line, Time received,
                                    Time now)
{
    tick(now);
    if (line == "STATUS") return status();
    if (!client_connected_) return "ERR NO_CLIENT\n";
    if (!tx_.monitor_healthy()) return "ERR MONITOR_FAILED\n";
    if (line.compare(0, 14, "SELECT_SOURCE ") == 0) {
        const std::string name = line.substr(14);
        uint8_t byte = 0;
        if (name == "CD") byte = 0x84;
        else if (name == "TUNER") byte = 0x82;
        else if (name == "PHONO") byte = 0x83;
        else if (name == "TAPE1") byte = 0x85;
        else if (name == "TV1" || name == "TV2" || name == "TAPE2")
            return "ERR SOURCE_UNVERIFIED\n";
        else return "ERR UNKNOWN_SOURCE\n";
        if (volume_.state() != VolumeControl::State::IDLE ||
            volume_.stop_owed() || volume_.context_may_be_open())
            return std::string("ERR ") + state_name(volume_.state()) + "\n";
        const bool verified = tx_.send(byte);
        const ServiceTxEvidence evidence = tx_.last_evidence();
        return std::string(verified ? "OK SOURCE_TX " : "ERR SOURCE_UNCERTAIN ") +
            "gpio_tx=" + (evidence.gpio_completed ? "TX_OK" : "TX_FAIL") +
            " waveform=" + waveform_name(evidence.waveform) +
            " receiver_execution=UNVERIFIED\n";
    }
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

std::string Xs8ServiceCore::begin_query_status(Time now)
{
    const VolumeControl::State received_state = volume_.state();
    const bool received_unresolved =
        volume_.stop_owed() || volume_.context_may_be_open();
    tick(now);
    if (!client_connected_) return "ERR NO_CLIENT\n";
    if (!tx_.monitor_healthy()) return "ERR MONITOR_FAILED\n";
    if (received_state != VolumeControl::State::IDLE)
        return std::string("ERR ") + state_name(received_state) + "\n";
    if (received_unresolved)
        return "ERR QUERY_STATUS reason=UNRESOLVED_STATE\n";
    if (volume_.state() != VolumeControl::State::IDLE ||
        volume_.stop_owed() || volume_.context_may_be_open())
        return "ERR QUERY_STATUS reason=UNRESOLVED_STATE\n";
    const bool verified = tx_.send(0x13);
    const ServiceTxEvidence evidence = tx_.last_evidence();
    if (!verified)
        return std::string("ERR QUERY_STATUS reason=TX_UNCERTAIN gpio_tx=") +
            (evidence.gpio_completed ? "TX_OK" : "TX_FAIL") +
            " waveform=" + waveform_name(evidence.waveform) +
            " receiver_execution=UNVERIFIED\n";
    return {};
}

std::string Xs8ServiceCore::research_ready(Time now)
{
    const auto before = volume_.state();
    const bool unresolved = volume_.stop_owed() ||
                            volume_.context_may_be_open();
    tick(now);
    if (!client_connected_) return "ERR RESEARCH reason=NO_CLIENT\n";
    if (!tx_.monitor_healthy()) return "ERR RESEARCH reason=MONITOR_FAILED\n";
    if (before != VolumeControl::State::IDLE ||
        volume_.state() != VolumeControl::State::IDLE)
        return std::string("ERR RESEARCH reason=") + state_name(before) + "\n";
    if (unresolved || volume_.stop_owed() || volume_.context_may_be_open())
        return "ERR RESEARCH reason=UNRESOLVED_STATE\n";
    return {};
}

std::string Xs8ServiceCore::research_send(uint8_t byte, Time now)
{
    std::string denied = research_ready(now);
    if (!denied.empty()) return denied;
    const bool verified = tx_.send(byte);
    const ServiceTxEvidence evidence = tx_.last_evidence();
    return std::string(verified ? "OK RESEARCH_TX" : "ERR RESEARCH_TX") +
        " gpio_tx=" + (evidence.gpio_completed ? "TX_OK" : "TX_FAIL") +
        " waveform=" + waveform_name(evidence.waveform) +
        " receiver_execution=UNVERIFIED";
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
