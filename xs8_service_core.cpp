#include "xs8_service_core.h"

#include <chrono>
#include <charconv>
#include <cstdio>
#include <random>
#include <sstream>
#include <thread>

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
    case State::RESEARCH_CONTEXT: return "RESEARCH_CONTEXT";
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

bool cd_token(const std::string& line, const char *prefix, uint64_t& token)
{
    const size_t length = std::char_traits<char>::length(prefix);
    if (line.size() <= length || line.compare(0, length, prefix) != 0)
        return false;
    const char *first = line.data() + length;
    const char *last = line.data() + line.size();
    const auto parsed = std::from_chars(first, last, token);
    return parsed.ec == std::errc{} && parsed.ptr == last && token != 0;
}

// "0xNN" exactly, as the research console and service parsers spell bytes.
bool context_byte(const std::string& text, uint8_t& byte)
{
    if (text.size() != 4 || text[0] != '0' || text[1] != 'x') return false;
    unsigned int value = 0;
    const auto parsed = std::from_chars(text.data() + 2, text.data() + 4, value, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + 4) return false;
    byte = static_cast<uint8_t>(value);
    return true;
}

std::string evidence_fields(const char *prefix, const ServiceTxEvidence& evidence)
{
    return std::string(" ") + prefix + "gpio_tx=" +
        (evidence.gpio_completed ? "TX_OK" : "TX_FAIL") + " " + prefix +
        "waveform=" + waveform_name(evidence.waveform);
}

bool new_cd_token(uint64_t previous, uint64_t& token)
{
    try {
        std::random_device random;
        for (unsigned int attempt = 0; attempt < 8; ++attempt) {
            token = (uint64_t(random()) << 32) | uint64_t(random());
            if (token != 0 && token != previous) return true;
        }
    } catch (...) {}
    return false;
}
}

Xs8ServiceCore::Xs8ServiceCore(ServiceTransmitter& tx,
    VolumeControl::Ms max_hold, bool operator_authorized,
    std::function<bool()> stopping, std::function<Time()> now,
    std::function<void(Time)> wait_until, VolumeControl::Ms cd_lease)
    : tx_(tx), volume_(tx, VolumeControl::Ms(1200), max_hold,
                       std::move(stopping), std::move(now)),
      wait_until_(wait_until ? std::move(wait_until) :
          std::function<void(Time)>([](Time deadline) {
              std::this_thread::sleep_until(deadline);
          })), cd_lease_(cd_lease)
{
    // This sends no command. No automatic recovery is available in phase 1.
    volume_.startup(false, operator_authorized);
}

void Xs8ServiceCore::connected(uint64_t client_id)
{
    clients_.insert(client_id);
}

std::string Xs8ServiceCore::status() const
{
    ServiceTxEvidence evidence = tx_.last_evidence();
    ServiceTxEvidence stop = tx_.last_stop_evidence();
    char last_byte[8];
    if (evidence.attempted)
        std::snprintf(last_byte, sizeof(last_byte), "0x%02X", evidence.byte);
    else std::snprintf(last_byte, sizeof(last_byte), "NONE");
    return std::string("OK state=") +
        (cd_fault_ ? "FAULT_LATCHED" : state_name(volume_.state())) +
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
        " cd_state=" + (cd_fault_ ? "FAULT_LATCHED" :
                         cd_hold_ ? "HOLDING" :
                         cd_active_ ? "ACTIVE" : "IDLE") +
        " cd_action=" + cd_action_ +
        " cd_release_owed=" + (cd_release_owed_ ? "1" : "0") +
        " cd_lease_expired=" + (cd_lease_expired_ ? "1" : "0") +
        " cd_last_result=" + cd_last_result_ +
        receiver_status_ + "\n";
}

bool Xs8ServiceCore::attempt_cd_release()
{
    bool verified = false;
    bool threw = false;
    try { verified = tx_.send(0x72); }
    catch (...) { threw = true; }
    cd_release_evidence_ = threw ? ServiceTxEvidence{} : tx_.last_evidence();
    const auto evidence = cd_release_evidence_;
    if (verified && evidence.attempted && evidence.byte == 0x72 &&
        evidence.gpio_completed && evidence.waveform == WaveformEvidence::MATCH)
        cd_release_owed_ = false;
    else {
        cd_fault_ = true;
        std::fprintf(stderr,
            "WARNING: CD KEY RELEASE 0x72 unconfirmed; inspect changer and "
            "recover physically before restarting authorized control\n");
    }
    return !cd_release_owed_;
}

std::string Xs8ServiceCore::cd_preflight() const
{
    if (cd_shutdown_requested_) return "ERR SHUTDOWN\n";
    if (cd_fault_ || cd_release_owed_) return "ERR CD_FAULT_LATCHED\n";
    if (volume_.state() != VolumeControl::State::IDLE ||
        volume_.stop_owed() || volume_.context_may_be_open())
        return std::string("ERR ") + state_name(volume_.state()) + "\n";
    return {};
}

std::string Xs8ServiceCore::cd_press(uint8_t press_byte, const char *action,
                                     Time now)
{
    const std::string denied = cd_preflight();
    if (!denied.empty()) return denied;
    uint64_t token = 0;
    // Generate independently before KEY DOWN. A prior owner's token must not
    // reveal the next token now that requests can use different connections.
    if (!new_cd_token(cd_last_token_, token)) return "ERR CD_TOKEN_UNAVAILABLE\n";
    // Expiry records the previous lease, not an unresolved RELEASE. Preflight
    // still rejects faults/obligations; only an accepted new operation clears it.
    cd_lease_expired_ = false;
    cd_active_ = true;
    cd_release_owed_ = true;
    cd_action_ = action;
    bool verified = false;
    bool threw = false;
    try { verified = tx_.send(press_byte); }
    catch (...) { threw = true; }
    const auto press = threw ? ServiceTxEvidence{} : tx_.last_evidence();
    const bool confirmed = verified && press.attempted &&
        press.byte == press_byte && press.gpio_completed &&
        press.waveform == WaveformEvidence::MATCH;
    if (!confirmed) {
        const bool released = attempt_cd_release();
        cd_active_ = false;
        cd_fault_ = true;
        cd_last_result_ = released ? "PRESS_UNCERTAIN" : "RELEASE_UNCERTAIN";
        std::fprintf(stderr, "XS8 CD_PRESS action=%s result=%s gpio_tx=%s "
                             "waveform=%s release_gpio_tx=%s "
                             "release_waveform=%s receiver_execution=UNVERIFIED\n",
            action, cd_last_result_.c_str(),
            press.gpio_completed ? "TX_OK" : "TX_FAIL",
            waveform_name(press.waveform),
            cd_release_evidence_.gpio_completed ? "TX_OK" : "TX_FAIL",
            waveform_name(cd_release_evidence_.waveform));
        return std::string("ERR CD_PRESS_UNCERTAIN action=") + action +
            " gpio_tx=" + (press.gpio_completed ? "TX_OK" : "TX_FAIL") +
            " waveform=" + waveform_name(press.waveform) +
            " release_gpio_tx=" +
            (cd_release_evidence_.gpio_completed ? "TX_OK" : "TX_FAIL") +
            " release_waveform=" + waveform_name(cd_release_evidence_.waveform) +
            " receiver_execution=UNVERIFIED\n";
    }
    cd_lease_renewed_ = now;
    cd_hold_token_ = token;
    cd_last_token_ = token;
    cd_hold_ = true;
    cd_last_result_ = "HOLDING";
    std::fprintf(stderr, "XS8 CD_PRESS action=%s gpio_tx=TX_OK "
                         "waveform=MATCH receiver_execution=UNVERIFIED\n",
                         action);
    return std::string("OK CD_PRESS_TX action=") + action +
        " hold_token=" + std::to_string(cd_hold_token_) +
        " gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED\n";
}

std::string Xs8ServiceCore::cd_release(const char *reason)
{
    if (!cd_hold_) return "ERR CD_NOT_ACTIVE\n";
    const bool verified = attempt_cd_release();
    cd_hold_ = false;
    cd_active_ = false;
    cd_hold_token_ = 0;
    cd_last_result_ = verified ? reason : "RELEASE_UNCERTAIN";
    std::fprintf(stderr, "XS8 CD_RELEASE reason=%s gpio_tx=%s waveform=%s "
                         "receiver_execution=UNVERIFIED\n", reason,
        cd_release_evidence_.gpio_completed ? "TX_OK" : "TX_FAIL",
        waveform_name(cd_release_evidence_.waveform));
    return std::string(verified ? "OK CD_RELEASE_TX" :
                       "ERR CD_RELEASE_UNCERTAIN") +
        " reason=" + reason +
        " gpio_tx=" +
        (cd_release_evidence_.gpio_completed ? "TX_OK" : "TX_FAIL") +
        " waveform=" + waveform_name(cd_release_evidence_.waveform) +
        " receiver_execution=UNVERIFIED\n";
}

std::string Xs8ServiceCore::cd_track(uint8_t press_byte, const char *action)
{
    const std::string denied = cd_preflight();
    if (!denied.empty()) return denied;
    cd_lease_expired_ = false;
    cd_active_ = true;
    cd_action_ = action;
    cd_release_owed_ = true; // Set before PRESS: failure may still reach the bus.
    bool press_verified = false;
    bool press_threw = false;
    try { press_verified = tx_.send(press_byte); }
    catch (...) { press_threw = true; }
    const auto press = press_threw ? ServiceTxEvidence{} : tx_.last_evidence();
    const Time press_end = press_threw ? Time{} : tx_.last_tx_end_time();
    bool gap_ready = press_end != Time{};
    const bool press_confirmed = press_verified && press.attempted &&
        press.byte == press_byte && press.gpio_completed &&
        press.waveform == WaveformEvidence::MATCH && gap_ready;
    if (press_confirmed) {
        // Nominal TX-end-to-next-TX-start gap. If verification consumed the
        // deadline, RELEASE follows immediately; no hard maximum is promised.
        try { wait_until_(press_end + VolumeControl::Ms(100)); }
        catch (...) { gap_ready = false; } // RELEASE still takes priority.
    }
    const bool release_verified = cd_shutdown_requested_ ? !cd_release_owed_ :
        (cd_release_owed_ ? attempt_cd_release() : true);
    const auto release = cd_release_evidence_;
    cd_active_ = false;
    if (!release_verified) {
        cd_last_result_ = "RELEASE_UNCERTAIN";
    } else if (!press_confirmed || !gap_ready) {
        cd_fault_ = true;
        cd_last_result_ = "PRESS_UNCERTAIN";
    } else cd_last_result_ = "TX_VERIFIED";
    std::fprintf(stderr,
        "XS8 CD_TRACK action=%s result=%s press_gpio_tx=%s "
        "press_waveform=%s release_gpio_tx=%s release_waveform=%s "
        "receiver_execution=UNVERIFIED\n", action, cd_last_result_.c_str(),
        press.gpio_completed ? "TX_OK" : "TX_FAIL",
        waveform_name(press.waveform),
        release.gpio_completed ? "TX_OK" : "TX_FAIL",
        waveform_name(release.waveform));
    return std::string(cd_last_result_ == "TX_VERIFIED" ?
        "OK CD_TRACK_TX" : "ERR CD_TRACK_UNCERTAIN") +
        " action=" + action +
        " press_gpio_tx=" + (press.gpio_completed ? "TX_OK" : "TX_FAIL") +
        " press_waveform=" + waveform_name(press.waveform) +
        " release_gpio_tx=" + (release.gpio_completed ? "TX_OK" : "TX_FAIL") +
        " release_waveform=" + waveform_name(release.waveform) +
        " cd_result=" + cd_last_result_ +
        " receiver_execution=UNVERIFIED\n";
}

std::string Xs8ServiceCore::command(const std::string& line, Time received,
                                    Time now, uint64_t client_id)
{
    tick(now);
    if (line == "STATUS") return status();
    if (!clients_.count(client_id)) return "ERR NO_CLIENT\n";
    if (line.compare(0, 16, "CONTEXT_SESSION_") == 0)
        return context_command(line, now, client_id);
    // No other XS8 transmission while research CONTEXT may be open. RELEASE
    // stays available; it transmits nothing without an active hold.
    if (context_open_) {
        // No hold can exist here; answer as IDLE does, without any TX.
        if (line == "VOLUME_RELEASE UP" || line == "VOLUME_RELEASE DOWN")
            return "OK VOLUME_RELEASE_TX\n";
        return "ERR RESEARCH_CONTEXT_ACTIVE\n";
    }
    if (line == "CD_RELEASE" || line.compare(0, 11, "CD_RELEASE ") == 0 ||
        line == "CD_HEARTBEAT" ||
        line.compare(0, 13, "CD_HEARTBEAT ") == 0) {
        if (!cd_hold_) return "ERR CD_NOT_ACTIVE\n";
        uint64_t token = 0;
        const bool heartbeat = line.compare(0, 13, "CD_HEARTBEAT ") == 0;
        if (!cd_token(line, heartbeat ? "CD_HEARTBEAT " : "CD_RELEASE ",
                      token)) return "ERR CD_TOKEN_REQUIRED\n";
        if (token != cd_hold_token_) return "ERR CD_NOT_OWNER\n";
        if (heartbeat) {
            cd_lease_renewed_ = now;
            return "OK CD_LEASE_RENEWED\n";
        }
        return cd_release("CLIENT_RELEASE");
    }
    if (cd_active_) return "ERR CD_TRACK_BUSY\n";
    if (cd_fault_ && line != "VOLUME_RELEASE UP" &&
        line != "VOLUME_RELEASE DOWN") return "ERR CD_FAULT_LATCHED\n";
    if (!tx_.monitor_healthy()) return "ERR MONITOR_FAILED\n";
    if (line == "CD_PRESS TRACK_NEXT")
        return cd_press(0xFC, "TRACK_NEXT", now);
    if (line == "CD_PRESS TRACK_PREVIOUS")
        return cd_press(0xEC, "TRACK_PREVIOUS", now);
    if (line == "CD_PRESS SEEK_FORWARD")
        return cd_press(0x7A, "SEEK_FORWARD", now);
    if (line == "CD_PRESS SEEK_BACKWARD")
        return cd_press(0x6A, "SEEK_BACKWARD", now);
    if (line.compare(0, 9, "CD_PRESS ") == 0)
        return "ERR CD_UNKNOWN_ACTION\n";
    if (line == "CD_TRACK_NEXT") return cd_track(0xFC, "NEXT");
    if (line == "CD_TRACK_PREVIOUS") return cd_track(0xEC, "PREVIOUS");
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
    if (line == "MUTE") {
        volume_owner_client_ = client_id;
        volume_owner_set_ = true;
        const bool sent = volume_.mute(now);
        if (volume_.state() == VolumeControl::State::IDLE &&
            !volume_.stop_owed() && !volume_.context_may_be_open())
            volume_owner_set_ = false;
        return sent ? "OK MUTE_TX\n" : "ERR MUTE_UNCERTAIN\n";
    }
    const bool press = line == "VOLUME_PRESS UP" ||
                       line == "VOLUME_PRESS DOWN";
    const bool release = line == "VOLUME_RELEASE UP" ||
                         line == "VOLUME_RELEASE DOWN";
    if (!press && !release) return "ERR UNKNOWN_COMMAND\n";
    VolumeControl::Direction direction = line.size() >= 2 &&
        line.compare(line.size() - 2, 2, "UP") == 0
        ? VolumeControl::Direction::UP : VolumeControl::Direction::DOWN;
    if (press) {
        if (volume_owner_set_ && volume_owner_client_ != client_id &&
            (volume_.state() == VolumeControl::State::HOLDING_UP ||
             volume_.state() == VolumeControl::State::HOLDING_DOWN))
            return "ERR VOLUME_BUSY\n";
        volume_owner_client_ = client_id;
        volume_owner_set_ = true;
        if (!volume_.press(direction, received, now)) {
            if (volume_.state() == VolumeControl::State::IDLE &&
                !volume_.stop_owed() && !volume_.context_may_be_open())
                volume_owner_set_ = false;
            return std::string("ERR ") + state_name(volume_.state()) + "\n";
        }
        owner_direction_ = direction;
        volume_owner_client_ = client_id;
        volume_owner_set_ = true;
        return "OK VOLUME_PRESS_TX\n";
    }
    return volume_.release(direction, now) ? "OK VOLUME_RELEASE_TX\n"
                                           : "ERR VOLUME_STOP_UNCERTAIN\n";
}

std::string Xs8ServiceCore::begin_query_status(Time now)
{
    if (cd_active_) return "ERR CD_TRACK_BUSY\n";
    if (cd_fault_ || cd_release_owed_) return "ERR CD_FAULT_LATCHED\n";
    const VolumeControl::State received_state = volume_.state();
    const bool received_unresolved =
        volume_.stop_owed() || volume_.context_may_be_open();
    tick(now);
    if (clients_.empty()) return "ERR NO_CLIENT\n";
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

std::string Xs8ServiceCore::research_ready(Time now, bool accepted_session)
{
    if (cd_active_) return "ERR RESEARCH reason=CD_TRACK_BUSY\n";
    if (cd_fault_ || cd_release_owed_) return "ERR RESEARCH reason=CD_FAULT_LATCHED\n";
    const auto before = volume_.state();
    const bool unresolved = volume_.stop_owed() ||
                            volume_.context_may_be_open();
    tick(now);
    // A previously authorized chained session outlives its request socket.
    if (clients_.empty() && !accepted_session)
        return "ERR RESEARCH reason=NO_CLIENT\n";
    if (!tx_.monitor_healthy()) return "ERR RESEARCH reason=MONITOR_FAILED\n";
    if (before != VolumeControl::State::IDLE ||
        volume_.state() != VolumeControl::State::IDLE)
        return std::string("ERR RESEARCH reason=") + state_name(before) + "\n";
    if (unresolved || volume_.stop_owed() || volume_.context_may_be_open())
        return "ERR RESEARCH reason=UNRESOLVED_STATE\n";
    return {};
}

std::string Xs8ServiceCore::research_send(uint8_t byte, Time now,
                                         bool accepted_session)
{
    std::string denied = research_ready(now, accepted_session);
    if (!denied.empty()) return denied;
    const bool verified = tx_.send(byte);
    const ServiceTxEvidence evidence = tx_.last_evidence();
    return std::string(verified ? "OK RESEARCH_TX" : "ERR RESEARCH_TX") +
        " gpio_tx=" + (evidence.gpio_completed ? "TX_OK" : "TX_FAIL") +
        " waveform=" + waveform_name(evidence.waveform) +
        " receiver_execution=UNVERIFIED";
}

void Xs8ServiceCore::disconnected(Time now, uint64_t client_id)
{
    if (!clients_.erase(client_id)) return;
    if (context_open_ && context_owner_ == client_id)
        context_close(now, "OWNER_DISCONNECTED", false);
    if (!volume_owner_set_ || volume_owner_client_ != client_id) return;
    volume_owner_set_ = false;
    if (volume_.state() == VolumeControl::State::HOLDING_UP ||
        volume_.state() == VolumeControl::State::HOLDING_DOWN)
        volume_.disconnect(owner_direction_, now);
    else if (volume_.stop_owed() || volume_.context_may_be_open())
        volume_.shutdown(now);
}

void Xs8ServiceCore::tick(Time now)
{
    context_sync();
    if (context_open_) {
        // Service-owned exit; an unverified 0x07 latches FAULT in finish().
        if (!tx_.monitor_healthy())
            context_close(now, "MONITOR_FAILED", true);
        else if (now - context_renewed_ >= kContextLease)
            context_close(now, "LEASE_EXPIRED", false);
        else if (now - context_started_ >= kContextMaxLifetime)
            context_close(now, "MAX_LIFETIME", false);
    }
    if (cd_hold_ && !tx_.monitor_healthy()) {
        cd_release("MONITOR_FAILED");
        cd_fault_ = true;
    } else if (cd_hold_ && now - cd_lease_renewed_ >= cd_lease_) {
        cd_release("LEASE_EXPIRED");
        cd_lease_expired_ = true;
    }
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

bool Xs8ServiceCore::shutdown(Time now)
{
    cd_shutdown_requested_ = true;
    if (context_open_) context_close(now, "SHUTDOWN", false);
    if (cd_hold_) {
        cd_shutdown_release_attempted_ = true;
        cd_release("SHUTDOWN");
    }
    // A graceful exit makes one last RELEASE attempt if the first was not
    // confirmed. A crash or power loss cannot provide this guarantee.
    if (cd_release_owed_ && !cd_shutdown_release_attempted_) {
        cd_shutdown_release_attempted_ = true;
        attempt_cd_release();
    }
    return volume_.shutdown(now) && !cd_release_owed_ && !cd_fault_;
}

void Xs8ServiceCore::context_sync()
{
    // Another cleanup path (e.g. VolumeControl shutdown) may have closed it.
    if (context_open_ && volume_.state() != VolumeControl::State::RESEARCH_CONTEXT) {
        context_open_ = false;
        context_token_ = 0;
        context_last_ = volume_.context_may_be_open() ? "CLOSED_UNCERTAIN"
                                                       : "CLOSED_BY_SERVICE";
    }
}

std::string Xs8ServiceCore::context_close(Time now, const char *reason,
                                          bool fault_after)
{
    const bool exited = volume_.research_context_exit(now, fault_after);
    const ServiceTxEvidence root = tx_.last_evidence();
    context_open_ = false;
    context_token_ = 0;
    const bool open = volume_.context_may_be_open();
    context_last_ = std::string(reason) + (open ? "_ROOT_UNCERTAIN" : "_ROOT_TX_OK");
    std::fprintf(stderr, "XS8 CONTEXT_SESSION exit reason=%s root_attempts=%u "
        "root_gpio_tx=%s root_waveform=%s context_may_be_open=%d state=%s "
        "sends=%u receiver_execution=UNVERIFIED\n", reason,
        volume_.last_root_attempts(), root.gpio_completed ? "TX_OK" : "TX_FAIL",
        waveform_name(root.waveform), open ? 1 : 0, state_name(volume_.state()),
        context_sends_);
    if (open || volume_.state() == VolumeControl::State::FAULT_LATCHED)
        std::fprintf(stderr, "WARNING: research CONTEXT exit unverified or fault "
            "latched; check the receiver physically before restarting\n");
    return " root_attempts=" + std::to_string(volume_.last_root_attempts()) +
        evidence_fields("root_", root) +
        " context_may_be_open=" + (open ? "1" : "0") +
        " state=" + state_name(volume_.state()) +
        (exited ? " root_verified=NOT_OBSERVED" : "") +
        " receiver_execution=UNVERIFIED\n";
}

std::string Xs8ServiceCore::context_command(const std::string& line, Time now,
                                            uint64_t client_id)
{
    std::istringstream input(line);
    std::string operation, first, second, extra;
    input >> operation >> first >> second >> extra;
    if (operation == "CONTEXT_SESSION_STATUS" && first.empty()) {
        const auto left = [&](Time from, VolumeControl::Ms limit) {
            const auto used = std::chrono::duration_cast<VolumeControl::Ms>(now - from);
            return std::to_string(context_open_ && used < limit
                                  ? (limit - used).count() : 0);
        };
        return std::string("OK CONTEXT_SESSION_STATUS open=") +
            (context_open_ ? "1" : "0") + " enabled=" + (context_enabled_ ? "1" : "0") +
            " sends=" + std::to_string(context_sends_) +
            " lease_remaining_ms=" + left(context_renewed_, kContextLease) +
            " lifetime_remaining_ms=" + left(context_started_, kContextMaxLifetime) +
            " last=" + context_last_ + " state=" + state_name(volume_.state()) +
            " context_may_be_open=" + (volume_.context_may_be_open() ? "1" : "0") + "\n";
    }
    if (operation == "CONTEXT_SESSION_ENTER") {
        if (first != "CONFIRM_ATTENDED" || !second.empty())
            return "ERR CONTEXT_SESSION reason=INVALID_REQUEST\n";
        if (!context_enabled_) return "ERR CONTEXT_SESSION reason=DISABLED\n";
        if (context_open_) return "ERR CONTEXT_SESSION reason=BUSY\n";
        if (cd_active_ || cd_hold_) return "ERR CONTEXT_SESSION reason=CD_BUSY\n";
        if (cd_fault_ || cd_release_owed_)
            return "ERR CONTEXT_SESSION reason=CD_FAULT_LATCHED\n";
        if (!tx_.monitor_healthy()) return "ERR CONTEXT_SESSION reason=MONITOR_FAILED\n";
        if (volume_.state() != VolumeControl::State::IDLE)
            return std::string("ERR CONTEXT_SESSION reason=") +
                state_name(volume_.state()) + "\n";
        if (volume_.stop_owed() || volume_.context_may_be_open())
            return "ERR CONTEXT_SESSION reason=UNRESOLVED_STATE\n";
        uint64_t token = 0;
        if (!new_cd_token(context_last_token_, token))
            return "ERR CONTEXT_SESSION reason=TOKEN_UNAVAILABLE\n";
        context_sends_ = 0;
        const bool entered = volume_.research_context_enter(now);
        const ServiceTxEvidence entry = tx_.last_evidence();
        if (!entered) {
            // research_context_enter() already attempted 0x07 and latched FAULT.
            context_last_ = "ENTER_UNCERTAIN";
            std::fprintf(stderr, "XS8 CONTEXT_SESSION enter result=UNCERTAIN\n");
            return std::string("ERR CONTEXT_SESSION_ENTER_UNCERTAIN byte=0x17") +
                " root_attempts=" + std::to_string(volume_.last_root_attempts()) +
                " context_may_be_open=" + (volume_.context_may_be_open() ? "1" : "0") +
                " state=" + state_name(volume_.state()) +
                " receiver_execution=UNVERIFIED\n";
        }
        context_open_ = true;
        context_token_ = context_last_token_ = token;
        context_owner_ = client_id;
        context_started_ = context_renewed_ = now;
        context_last_ = "OPEN";
        std::fprintf(stderr, "XS8 CONTEXT_SESSION enter result=TX_VERIFIED "
                             "receiver_execution=UNVERIFIED\n");
        return "OK CONTEXT_SESSION_ENTERED token=" + std::to_string(token) +
            " byte=0x17" + evidence_fields("", entry) + " context_may_be_open=1" +
            " lease_ms=" + std::to_string(kContextLease.count()) +
            " max_ms=" + std::to_string(kContextMaxLifetime.count()) +
            " receiver_execution=UNVERIFIED\n";
    }
    if (operation != "CONTEXT_SESSION_SEND" && operation != "CONTEXT_SESSION_EXIT" &&
        operation != "CONTEXT_SESSION_HEARTBEAT")
        return "ERR CONTEXT_SESSION reason=INVALID_REQUEST\n";
    const bool sending = operation == "CONTEXT_SESSION_SEND";
    uint64_t token = 0;
    uint8_t byte = 0;
    const auto parsed = std::from_chars(first.data(), first.data() + first.size(), token);
    if (first.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != first.data() + first.size() || token == 0 || !extra.empty() ||
        (sending ? !context_byte(second, byte) : !second.empty()))
        return "ERR CONTEXT_SESSION reason=INVALID_REQUEST\n";
    if (!context_open_) return "ERR CONTEXT_SESSION reason=NOT_ACTIVE\n";
    // Ownership is the token on the connection that entered; neither alone.
    if (token != context_token_ || client_id != context_owner_)
        return "ERR CONTEXT_SESSION reason=NOT_OWNER\n";
    context_renewed_ = now;
    if (operation == "CONTEXT_SESSION_HEARTBEAT")
        return "OK CONTEXT_SESSION_LEASE_RENEWED\n";
    if (!sending) {
        const std::string fields = context_close(now, "CLIENT_EXIT", false);
        const bool uncertain = volume_.context_may_be_open() ||
            volume_.state() == VolumeControl::State::FAULT_LATCHED;
        return std::string(uncertain ? "ERR CONTEXT_SESSION_EXIT_UNCERTAIN"
                                     : "OK CONTEXT_SESSION_EXITED") + " byte=0x07" + fields;
    }
    char spelled[8];
    std::snprintf(spelled, sizeof(spelled), "0x%02X", byte);
    // Not an allowlist: these have dedicated paths or known continuing effects.
    if (byte == 0x17) return "ERR CONTEXT_SESSION reason=NESTED_ENTRY_FORBIDDEN\n";
    if (byte == 0x07) return "ERR CONTEXT_SESSION reason=USE_EXIT\n";
    if (byte == 0xE9 || byte == 0xF9)
        return "ERR CONTEXT_SESSION reason=VOLUME_MOTOR_FORBIDDEN\n";
    ++context_sends_;
    std::fprintf(stderr, "XS8 CONTEXT_SESSION send requested=%s\n", spelled);
    const bool verified = volume_.research_context_send(byte);
    const ServiceTxEvidence candidate = tx_.last_evidence();
    if (verified)
        return std::string("OK CONTEXT_SESSION_SENT byte=") + spelled +
            evidence_fields("", candidate) + " context_may_be_open=1" +
            " receiver_execution=UNVERIFIED\n";
    // Never retried: the byte may have reached the receiver. Exit and latch.
    return std::string("ERR CONTEXT_SESSION_SEND_UNCERTAIN byte=") + spelled +
        evidence_fields("", candidate) + context_close(now, "SEND_UNCERTAIN", true);
}
