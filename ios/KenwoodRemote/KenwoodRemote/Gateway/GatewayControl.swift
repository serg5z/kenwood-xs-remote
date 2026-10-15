import Foundation

// The Gateway's control endpoints (docs/web-api.md). Each command maps to one
// documented request with an allowlisted body; the app never sends XS8 bytes.

enum HoldAction: Equatable, Sendable {
    case start(direction: String)
    case heartbeat(holdId: String)
    /// Stop-only. Without a hold id it releases this session's hold (used after
    /// an uncertain START); with one, a stale id can never stop a newer hold.
    case release(holdId: String?)
}

enum PlaybackAction: String, CaseIterable, Sendable {
    case playPause = "PLAY_PAUSE", stop = "STOP", timeDisplay = "TIME_DISPLAY"
    case repeatMode = "REPEAT", discSkip = "DISC_SKIP"

    var label: String {
        switch self {
        case .playPause: "PLAY/PAUSE"
        case .stop: "STOP"
        case .timeDisplay: "TIME DISP."
        case .repeatMode: "REPEAT"
        case .discSkip: "DISC SKIP"
        }
    }
}

enum ControlCommand: Equatable, Sendable {
    /// `expected` only guards (a fresh ON or OFF_INFERRED shown on screen); the
    /// Gateway's fresh observation always decides.
    case powerToggle(expected: String?)
    case power(on: Bool)
    case source(Source)
    case tape2Monitor
    case mute
    case receiverQuery
    case volumeStep(up: Bool)
    case volumeHold(HoldAction)
    case cdTrack(next: Bool)
    case cdPlayback(PlaybackAction)
    case cdSeek(HoldAction)
    case cdTrackSelect(Int)

    var path: String {
        switch self {
        case .powerToggle, .power: "/api/v1/power"
        case .source: "/api/v1/source"
        case .tape2Monitor: "/api/v1/receiver/monitor"
        case .mute: "/api/v1/mute"
        case .receiverQuery: "/api/v1/receiver/query"
        case .volumeStep: "/api/v1/volume/step"
        case .volumeHold: "/api/v1/volume/hold"
        case .cdTrack: "/api/v1/cd/track"
        case .cdPlayback: "/api/v1/cd/playback"
        case .cdSeek: "/api/v1/cd/seek"
        case .cdTrackSelect: "/api/v1/cd/track/select"
        }
    }

    /// Client timeout: above the Gateway's worst case for the request, which
    /// stays under nginx's 14 s `proxy_read_timeout` (same values as the Web).
    var timeout: TimeInterval {
        switch self {
        case .powerToggle: 15
        case .source, .tape2Monitor, .cdTrackSelect: 12
        case .volumeHold, .cdSeek: 4
        default: 6
        }
    }

    /// The exact JSON body, or nil if a component is not valid (nothing is sent).
    var body: Data? {
        let json: String
        switch self {
        case .powerToggle(let expected?):
            guard ["ON", "OFF_INFERRED"].contains(expected) else { return nil }
            json = #"{"action":"TOGGLE","expected":"\#(expected)"}"#
        case .powerToggle(nil): json = #"{"action":"TOGGLE"}"#
        case .power(let on): json = #"{"action":"\#(on ? "ON" : "OFF")"}"#
        case .source(let source): json = #"{"source":"\#(source.rawValue)"}"#
        case .tape2Monitor, .mute, .receiverQuery: json = "{}"
        case .volumeStep(let up): json = #"{"direction":"\#(up ? "UP" : "DOWN")"}"#
        case .volumeHold(let action):
            guard let hold = Self.holdJSON(action, directions: ["UP", "DOWN"]) else { return nil }
            json = hold
        case .cdSeek(let action):
            guard let hold = Self.holdJSON(action, directions: ["FORWARD", "BACKWARD"]) else { return nil }
            json = hold
        case .cdTrack(let next): json = #"{"action":"\#(next ? "NEXT" : "PREVIOUS")"}"#
        case .cdPlayback(let action): json = #"{"action":"\#(action.rawValue)"}"#
        case .cdTrackSelect(let track):
            guard (1...39).contains(track) else { return nil }
            json = #"{"track":\#(track)}"#
        }
        let data = Data(json.utf8)
        // The Gateway refuses bodies over 64 bytes.
        return data.count <= 64 ? data : nil
    }

    private static func holdJSON(_ action: HoldAction, directions: [String]) -> String? {
        switch action {
        case .start(let direction):
            guard directions.contains(direction) else { return nil }
            return #"{"action":"START","direction":"\#(direction)"}"#
        case .heartbeat(let holdId):
            guard isValidHoldId(holdId) else { return nil }
            return #"{"action":"HEARTBEAT","hold_id":"\#(holdId)"}"#
        case .release(let holdId?):
            guard isValidHoldId(holdId) else { return nil }
            return #"{"action":"RELEASE","hold_id":"\#(holdId)"}"#
        case .release(nil):
            return #"{"action":"RELEASE"}"#
        }
    }

    /// The Gateway's own hold_id rule: 1–43 ASCII letters, digits, '-' or '_'.
    static func isValidHoldId(_ id: String) -> Bool {
        (1...43).contains(id.count) && id.allSatisfy {
            $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "-" || $0 == "_")
        }
    }
}

/// A control reply's fields, all optional: refusals carry only some of them.
struct ControlReply: Decodable, Sendable, Equatable {
    var result: String?
    var error: String?
    var physicalCheckRequired: Bool?
    var powerSent: Bool?
    var csrfToken: String?
    var holdId: String?
    var heartbeatMs: Int?
    var leaseMs: Int?
    var maxHoldMs: Int?
    var minIntervalMs: Int?
    var sync: SyncReply?
    var sent: String?
    var decision: Decision?
    var stage: String?
    var cdLastResult: String?
    var reason: String?
    var observedSource: String?
    var observedMonitor: String?
    var mapping: String?
    /// Present when leaving a fresh CD also sent CD_STOP (its evidence).
    var cdStop: Present?

    /// A JSON object whose contents are not needed, only its presence.
    struct Present: Decodable, Sendable, Equatable {}

    struct Decision: Decodable, Sendable, Equatable {
        var state: String?
    }

    enum CodingKeys: String, CodingKey {
        case result, error, sent, decision, stage, reason, mapping, sync
        case physicalCheckRequired = "physical_check_required"
        case powerSent = "power_sent"
        case csrfToken = "csrf_token"
        case holdId = "hold_id"
        case heartbeatMs = "heartbeat_ms"
        case leaseMs = "lease_ms"
        case maxHoldMs = "max_hold_ms"
        case minIntervalMs = "min_interval_ms"
        case cdLastResult = "cd_last_result"
        case observedSource = "observed_source"
        case observedMonitor = "observed_monitor"
        case cdStop = "cd_stop"
    }
}

/// The final SYNC after a verified source or MONITOR control.
struct SyncReply: Decodable, Sendable, Equatable {
    var result: String?
    var sinceMonoNs: UInt64?
    var observedSource: String?
    var observedMonitor: String?
    var match: Bool?
    var reason: String?

    enum CodingKeys: String, CodingKey {
        case result, match, reason
        case sinceMonoNs = "since_mono_ns"
        case observedSource = "observed_source"
        case observedMonitor = "observed_monitor"
    }
}

/// What happened to one control request, before interpretation.
enum ControlExchange: Sendable, Equatable {
    /// Nothing was sent: the hold was released while START waited, no CSRF
    /// token could be obtained, or the request could not be built.
    case notAttempted(String)
    case transportFailed(TransportError)
    case response(status: Int, reply: ControlReply?, gatewayJSON: Bool)

    var reply: ControlReply? {
        if case .response(_, let reply, _) = self { return reply }
        return nil
    }

    var status: Int? {
        if case .response(let status, _, _) = self { return status }
        return nil
    }
}

/// The agreed outcome policy. HTTP 200 is a verified *transmission*, never a
/// confirmed physical action. Only a provably clean refusal is NOTHING SENT.
enum ControlOutcome: Equatable, Sendable {
    case txVerified(String)
    case nothingSent(String)
    case uncertain(String)
    case fault(String)

    static let originRejected = "HTTPS Origin required"

    init(_ exchange: ControlExchange) {
        switch exchange {
        case .notAttempted(let reason):
            self = .nothingSent(reason)
        case .transportFailed(let error):
            self = error.mayHaveReachedServer ? .uncertain(error.resultName) : .nothingSent(error.resultName)
        case .response(let status, let reply, let json):
            self = Self.classify(status: status, reply: reply, json: json)
        }
    }

    private static func classify(status: Int, reply: ControlReply?, json: Bool) -> ControlOutcome {
        let result = reply?.result
        let named = result ?? "HTTP \(status)"
        if status == 200 {
            // A 200 without a readable Gateway body cannot be interpreted.
            return json && reply != nil ? .txVerified(result ?? "SERVICE_TX_OK") : .uncertain("UNREADABLE REPLY")
        }
        if result == "FAULT_LATCHED" || result == "CD_FAULT_LATCHED" { return .fault(result!) }
        // The Gateway's own statement that the hardware may have moved, or that
        // a POWER frame went out, outranks the status code.
        if reply?.physicalCheckRequired == true || reply?.powerSent == true { return .uncertain(named) }
        switch status {
        case 403:
            // Origin is checked before the session and CSRF: nothing was read.
            // An invalid CSRF token may mean an earlier copy of this request
            // (an OS-level resend) already used it, so it is never "nothing sent".
            return json && reply?.error == originRejected
                ? .nothingSent("ORIGIN REJECTED") : .uncertain("INVALID CSRF TOKEN")
        case 401:
            return .nothingSent(json ? "SESSION EXPIRED" : "PASSWORD REJECTED")
        case 400:
            return .nothingSent("REQUEST REJECTED")
        case 429:
            return .nothingSent("TOO FAST")
        case 501:
            return .nothingSent("CONTROL DISABLED")
        case 409:
            return json ? .nothingSent(named) : .uncertain(named)
        case 503:
            // The Gateway's 503 is a refusal before dispatch (no service).
            return json ? .nothingSent(named) : .uncertain(named)
        default:
            // 502, 504 and anything unexpected.
            return .uncertain(named)
        }
    }

    var reason: String {
        switch self {
        case .txVerified(let r), .nothingSent(let r), .uncertain(let r), .fault(let r): r
        }
    }
}

extension TransportError {
    var resultName: String {
        switch self {
        case .tlsRejected: "TLS REJECTED"
        case .unreachable: "PI NOT REACHED"
        case .timedOut: "NO REPLY (TIMEOUT)"
        case .connectionLost: "CONNECTION LOST"
        case .cancelled: "REQUEST CANCELLED"
        case .other: "NETWORK ERROR"
        }
    }
}

/// Shared between the model and the client: once cancelled, a request still
/// waiting for its turn is never sent (a released button never starts a hold).
final class SendGuard: @unchecked Sendable {
    private let lock = NSLock()
    private var cancelled = false

    var isCancelled: Bool { lock.withLock { cancelled } }
    func cancel() { lock.withLock { cancelled = true } }
}
