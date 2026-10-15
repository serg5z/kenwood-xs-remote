import Foundation

/// Sources in screen order. TAPE2 is a MONITOR, not a source, and is not here.
enum Source: String, CaseIterable, Identifiable, Sendable {
    case cd = "CD", video1 = "VIDEO1", video2 = "VIDEO2"
    case tuner = "TUNER", phono = "PHONO", tape1 = "TAPE1"

    var id: String { rawValue }
    /// Only CD has a contextual panel in v1; TUNER controls are not validated.
    var hasPanel: Bool { self == .cd }
}

enum ObservationQuality: Equatable, Sendable {
    case fresh
    /// An older healthy observation (over 30 s): shown dimmed, never as current.
    case lastKnown
    /// Receiver integrity is uncertain: the value is not shown as a selection.
    case uncertain
    case unknown
    case connectionLost

    var label: String {
        switch self {
        case .fresh: "FRESH"
        case .lastKnown: "LAST KNOWN"
        case .uncertain: "UNCERTAIN"
        case .unknown: "UNKNOWN"
        case .connectionLost: "CONNECTION LOST"
        }
    }
}

enum SourceHighlight: Equatable, Sendable {
    case none, lastKnown, fresh
}

/// Everything shown about the receiver comes from Gateway observations, never
/// from a button press or a transmission result.
struct SourceIndicator: Equatable, Sendable {
    /// The raw observed value (may be TAPE2 or a value outside the six buttons).
    let observed: String?
    let quality: ObservationQuality
    let ageMs: Int?

    init(receiver: ReceiverObservations?, connected: Bool) {
        guard connected else { self.init(nil, .connectionLost, nil); return }
        guard let receiver, receiver.source.value != "UNKNOWN" else {
            self.init(nil, .unknown, nil); return
        }
        let source = receiver.source
        let quality: ObservationQuality =
            receiver.integrity != "HEALTHY" || source.freshness == "UNCERTAIN" ? .uncertain :
            source.freshness == "FRESH" ? .fresh :
            source.freshness == "LAST_KNOWN" ? .lastKnown : .unknown
        self.init(source.value, quality, source.ageMs)
    }

    private init(_ observed: String?, _ quality: ObservationQuality, _ ageMs: Int?) {
        self.observed = observed
        self.quality = quality
        self.ageMs = ageMs
    }

    /// The source the screen may treat as selected (for the panel), if any.
    var displayedSource: Source? {
        guard quality == .fresh || quality == .lastKnown, let observed else { return nil }
        return Source(rawValue: observed)
    }

    func highlight(for source: Source) -> SourceHighlight {
        guard displayedSource == source else { return .none }
        return quality == .fresh ? .fresh : .lastKnown
    }

    var summary: String {
        switch quality {
        case .connectionLost: "Observed source unavailable · connection lost"
        case .unknown: "Observed source unknown"
        case .uncertain: "Observed \(observed ?? "source") · observation uncertain"
        case .fresh: "Observed \(observed ?? "") · \(ageText)"
        case .lastKnown: "Last known \(observed ?? "") · \(ageText)"
        }
    }

    private var ageText: String { ageMs.map { "\(Self.age($0)) ago" } ?? "age unknown" }

    static func age(_ ms: Int) -> String {
        ms < 60_000 ? "\(ms / 1000) s" : "\(ms / 60_000) min"
    }
}

/// TAPE2 MONITOR shows ON/OFF only for a fresh, healthy observation (as on the
/// Web); anything else is neutral.
enum MonitorIndicator: Equatable, Sendable {
    case on, off, unknown

    init(receiver: ReceiverObservations?, connected: Bool) {
        guard connected, let receiver, receiver.integrity == "HEALTHY",
              receiver.monitor.freshness == "FRESH" else { self = .unknown; return }
        switch receiver.monitor.value {
        case "ON": self = .on
        case "OFF": self = .off
        default: self = .unknown
        }
    }
}

/// POWER shows a state only for a fresh Gateway observation.
enum PowerIndicator: Equatable, Sendable {
    case on, standbyInferred, unknown

    init(power: PowerObservation?, connected: Bool) {
        guard connected, let power, power.freshness == "FRESH" else { self = .unknown; return }
        switch power.value {
        case "ON": self = .on
        case "OFF_INFERRED": self = .standbyInferred
        default: self = .unknown
        }
    }

    var label: String {
        switch self {
        case .on: "Power on (observed)"
        case .standbyInferred: "Standby (inferred: no reply twice)"
        case .unknown: "Power state unknown"
        }
    }
}

/// The contextual panel follows the observed source: it expands when the
/// observed source changes to one with a panel, and stays as the user left it
/// otherwise. An unknown observation keeps the current panel, so the layout
/// does not jump when the connection drops.
struct ContextPanelState: Equatable, Sendable {
    private(set) var source: Source?
    private(set) var isExpanded = false

    var visiblePanel: Source? { source?.hasPanel == true ? source : nil }

    mutating func observe(_ observed: Source?) {
        guard let observed, observed != source else { return }
        source = observed
        isExpanded = observed.hasPanel
    }

    mutating func toggle() {
        guard visiblePanel != nil else { return }
        isExpanded.toggle()
    }
}
