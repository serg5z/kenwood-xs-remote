import Foundation

// Decoders for the Gateway's read endpoints (docs/web-api.md). Fields are
// optional where the Gateway may omit them, and unknown fields are ignored.
// Explicit CodingKeys throughout: convertFromSnakeCase would also rewrite
// dictionary keys such as "cd_track_select" and the STATUS field names.

/// `GET /api/v1/status`: 200 with `xs8_service: "connected"`, or 503 with
/// `xs8_service: "unavailable"` and an `error`.
struct StatusResponse: Decodable, Sendable, Equatable {
    let gateway: String?
    let xs8Service: String?
    let error: String?
    /// The service's STATUS fields, all strings (state, monitor, cd_state, …).
    let status: [String: String]?
    let receiver: ReceiverObservations?
    let cdChanger: CDChangerObservation?
    let power: PowerObservation?

    var serviceConnected: Bool { xs8Service == "connected" }

    enum CodingKeys: String, CodingKey {
        case gateway, error, status, receiver, power
        case xs8Service = "xs8_service"
        case cdChanger = "cd_changer"
    }
}

struct ReceiverObservation: Decodable, Sendable, Equatable {
    let value: String
    /// The Pi's monotonic clock: comparable only with other values from the
    /// same Gateway boot, never with iPhone time.
    let observedMonoNs: UInt64?
    let evidence: String?
    let ageMs: Int?
    /// FRESH, LAST_KNOWN, UNCERTAIN or UNKNOWN.
    let freshness: String

    enum CodingKeys: String, CodingKey {
        case value, evidence, freshness
        case observedMonoNs = "observed_mono_ns"
        case ageMs = "age_ms"
    }
}

struct ReceiverObservations: Decodable, Sendable, Equatable {
    let source: ReceiverObservation
    let monitor: ReceiverObservation
    /// HEALTHY or UNCERTAIN.
    let integrity: String
}

/// Gateway-owned and diagnostic: ON, OFF_INFERRED or UNKNOWN; freshness FRESH,
/// STALE or UNKNOWN. Changes only from the Gateway's own queries.
struct PowerObservation: Decodable, Sendable, Equatable {
    let value: String
    let observedMonoNs: UInt64?
    let ageMs: Int?
    let evidence: String?
    let freshness: String

    enum CodingKeys: String, CodingKey {
        case value, evidence, freshness
        case observedMonoNs = "observed_mono_ns"
        case ageMs = "age_ms"
    }
}

/// Diagnostic CD changer presence; null from a service without the probe.
struct CDChangerObservation: Decodable, Sendable, Equatable {
    let value: String
    let ageMs: Int?
    let lastProbe: String?

    enum CodingKeys: String, CodingKey {
        case value
        case ageMs = "age_ms"
        case lastProbe = "last_probe"
    }
}

/// `GET /api/v1/session`. The token stays inside GatewayClient; only the
/// availability flags reach the rest of the app.
struct SessionResponse: Decodable, Sendable {
    let csrfToken: String?
    let availability: SessionAvailability

    enum CodingKeys: String, CodingKey { case csrfToken = "csrf_token" }

    init(from decoder: Decoder) throws {
        csrfToken = try decoder.container(keyedBy: CodingKeys.self)
            .decodeIfPresent(String.self, forKey: .csrfToken)
        availability = try SessionAvailability(from: decoder)
    }
}

/// The per-control `*_available` flags from `GET /api/v1/session`.
struct SessionAvailability: Decodable, Sendable, Equatable {
    var mute = false
    var source = false
    var receiverQuery = false
    var cdTrack = false
    var volumeStep = false
    var cdPlayback = false
    var cdSeek = false
    var volumeHold = false
    var power = false
    var tape2Monitor = false
    var cdTrackSelect = false

    enum CodingKeys: String, CodingKey {
        case mute = "mute_available"
        case source = "source_available"
        case receiverQuery = "receiver_query_available"
        case cdTrack = "cd_track_available"
        case volumeStep = "volume_step_available"
        case cdPlayback = "cd_playback_available"
        case cdSeek = "cd_seek_available"
        case volumeHold = "volume_hold_available"
        case power = "power_available"
        case tape2Monitor = "tape2_monitor_available"
        case cdTrackSelect = "cd_track_select_available"
    }

    init() {}

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        func flag(_ key: CodingKeys) throws -> Bool { try c.decodeIfPresent(Bool.self, forKey: key) ?? false }
        mute = try flag(.mute)
        source = try flag(.source)
        receiverQuery = try flag(.receiverQuery)
        cdTrack = try flag(.cdTrack)
        volumeStep = try flag(.volumeStep)
        cdPlayback = try flag(.cdPlayback)
        cdSeek = try flag(.cdSeek)
        volumeHold = try flag(.volumeHold)
        power = try flag(.power)
        tape2Monitor = try flag(.tape2Monitor)
        cdTrackSelect = try flag(.cdTrackSelect)
    }
}

/// `GET /api/v1/capabilities`. `enabled` is true only for a valid session with
/// the gate on and an eligible STATUS.
struct Capabilities: Decodable, Sendable, Equatable {
    let apiVersion: String?
    let operations: [String: Operation]

    enum CodingKeys: String, CodingKey {
        case operations
        case apiVersion = "api_version"
    }

    func operation(_ name: OperationName) -> Operation? { operations[name.rawValue] }

    struct Operation: Decodable, Sendable, Equatable {
        let classification: String?
        let enabled: Bool
        let availableActions: [String]?
        let availableDirections: [String]?
        let availableSources: [String]?
        let blockedActions: [String]?
        let blockedSources: [String]?
        let provisionalActions: [String]?
        let minIntervalMs: Int?
        let heartbeatMs: Int?
        let leaseMs: Int?
        let maxHoldMs: Int?
        let holdDelayMs: Int?
        let toggleAvailable: Bool?
        let minTrack: Int?
        let maxTrack: Int?
        let frameGapMs: Int?
        let provisional: Bool?
        let reason: String?

        enum CodingKeys: String, CodingKey {
            case classification, enabled, provisional, reason
            case availableActions = "available_actions"
            case availableDirections = "available_directions"
            case availableSources = "available_sources"
            case blockedActions = "blocked_actions"
            case blockedSources = "blocked_sources"
            case provisionalActions = "provisional_actions"
            case minIntervalMs = "min_interval_ms"
            case heartbeatMs = "heartbeat_ms"
            case leaseMs = "lease_ms"
            case maxHoldMs = "max_hold_ms"
            case holdDelayMs = "hold_delay_ms"
            case toggleAvailable = "toggle_available"
            case minTrack = "min_track"
            case maxTrack = "max_track"
            case frameGapMs = "frame_gap_ms"
        }

        init(from decoder: Decoder) throws {
            let c = try decoder.container(keyedBy: CodingKeys.self)
            classification = try c.decodeIfPresent(String.self, forKey: .classification)
            enabled = try c.decodeIfPresent(Bool.self, forKey: .enabled) ?? false
            availableActions = try c.decodeIfPresent([String].self, forKey: .availableActions)
            availableDirections = try c.decodeIfPresent([String].self, forKey: .availableDirections)
            availableSources = try c.decodeIfPresent([String].self, forKey: .availableSources)
            blockedActions = try c.decodeIfPresent([String].self, forKey: .blockedActions)
            blockedSources = try c.decodeIfPresent([String].self, forKey: .blockedSources)
            provisionalActions = try c.decodeIfPresent([String].self, forKey: .provisionalActions)
            minIntervalMs = try c.decodeIfPresent(Int.self, forKey: .minIntervalMs)
            heartbeatMs = try c.decodeIfPresent(Int.self, forKey: .heartbeatMs)
            leaseMs = try c.decodeIfPresent(Int.self, forKey: .leaseMs)
            maxHoldMs = try c.decodeIfPresent(Int.self, forKey: .maxHoldMs)
            holdDelayMs = try c.decodeIfPresent(Int.self, forKey: .holdDelayMs)
            toggleAvailable = try c.decodeIfPresent(Bool.self, forKey: .toggleAvailable)
            minTrack = try c.decodeIfPresent(Int.self, forKey: .minTrack)
            maxTrack = try c.decodeIfPresent(Int.self, forKey: .maxTrack)
            frameGapMs = try c.decodeIfPresent(Int.self, forKey: .frameGapMs)
            provisional = try c.decodeIfPresent(Bool.self, forKey: .provisional)
            reason = try c.decodeIfPresent(String.self, forKey: .reason)
        }
    }
}

enum OperationName: String, CaseIterable, Sendable {
    case status, receiverQuery = "receiver_query", mute, volumeStep = "volume_step"
    case volumeHold = "volume_hold", source, power, tape2Monitor = "tape2_monitor"
    case cdTransport = "cd_transport", cdPlayback = "cd_playback", cdSeek = "cd_seek"
    case cdTrackSelect = "cd_track_select"
}
