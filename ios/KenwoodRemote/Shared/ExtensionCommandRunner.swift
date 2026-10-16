import Foundation

/// The Control Center write controls. Phase 1: MUTE and single volume steps.
enum ExtensionCommand: String, Sendable, CaseIterable {
    case mute, volumeUp, volumeDown

    var label: String {
        switch self {
        case .mute: "MUTE"
        case .volumeUp: "VOL+"
        case .volumeDown: "VOL−"
        }
    }

    var control: ControlCommand {
        switch self {
        case .mute: .mute
        case .volumeUp: .volumeStep(up: true)
        case .volumeDown: .volumeStep(up: false)
        }
    }

    var effect: String { self == .mute ? "RECEIVER ACTION" : "VOLUME CHANGE" }
    var isVolume: Bool { self != .mute }

    /// The Gateway's own availability for this control, from `/session`.
    func isAvailable(_ session: SessionAvailability) -> Bool {
        isVolume ? session.volumeStep : session.mute
    }
}

/// Runs one Control Center tap: `GET /session` (cookie check, fresh one-use
/// token, fresh availability), then at most one control POST, never retried.
/// One tap at a time per extension process; an overlapping tap is refused
/// with nothing sent. The extension keeps its own Gateway session, separate
/// from the app's, so the two never consume each other's CSRF tokens.
actor ExtensionCommandRunner {
    typealias CredentialLoader = @Sendable () throws -> BasicCredentials?
    typealias ClientFactory = @Sendable (BasicCredentials) throws -> GatewayClient

    /// The Gateway's volume spacing; also enforced (and authoritative) there.
    static let volumeIntervalMs = 250

    private let loadCredentials: CredentialLoader
    private let sessions: any SessionCookieStore
    private let makeClient: ClientFactory
    private let now: @Sendable () -> Date
    private var running = false
    private var lastVolumeCompleted: Date?
    private var invocations = 0
    private var cached: (credentials: BasicCredentials, client: GatewayClient)?

    init(loadCredentials: @escaping CredentialLoader, sessions: any SessionCookieStore,
         now: @escaping @Sendable () -> Date = { Date() }, makeClient: @escaping ClientFactory) {
        self.loadCredentials = loadCredentials
        self.sessions = sessions
        self.now = now
        self.makeClient = makeClient
    }

    func run(_ command: ExtensionCommand) async -> ControlDiagnostic {
        let started = ContinuousClock.now
        invocations += 1
        var record = ControlDiagnostic(date: now(), processID: getpid(), processAgeMs: nil,
                                       invocationInProcess: invocations, keychainMs: nil, requestMs: nil,
                                       totalMs: 0, ok: false, summary: "", command: command.label)
        func finish(_ outcome: ControlOutcome) -> ControlDiagnostic {
            record.ok = { if case .txVerified = outcome { return true }; return false }()
            record.summary = Self.summary(command, outcome)
            record.totalMs = Self.ms(since: started)
            return record
        }

        // Taps are dropped, never queued: a late step is worse than a lost one.
        guard !running else { return finish(.nothingSent("ANOTHER TAP IN PROGRESS")) }
        running = true
        defer { running = false }
        if command.isVolume, let last = lastVolumeCompleted,
           now().timeIntervalSince(last) * 1000 < Double(Self.volumeIntervalMs) {
            return finish(.nothingSent("TOO FAST"))
        }

        let keychainStarted = ContinuousClock.now
        let credentials: BasicCredentials
        do {
            guard let loaded = try loadCredentials() else { return finish(.nothingSent("NO CREDENTIALS · OPEN THE APP")) }
            credentials = loaded
        } catch let error as KeychainError {
            return finish(.nothingSent(error.isLocked ? "DEVICE LOCKED" : "KEYCHAIN ERROR \(error.status)"))
        } catch {
            return finish(.nothingSent("KEYCHAIN ERROR"))
        }
        record.keychainMs = Self.ms(since: keychainStarted)

        let client: GatewayClient
        do {
            client = try clientFor(credentials)
        } catch {
            return finish(.nothingSent("CA NOT USABLE"))
        }

        // The stored cookie only within its local lifetime; otherwise none, and
        // the Gateway issues a new session.
        let stored = sessions.load().flatMap { $0.isUsable(at: now()) ? $0 : nil }
        if stored == nil { sessions.delete() }
        await client.useSessionCookie(stored?.cookie)

        let sessionStarted = ContinuousClock.now
        let session = await client.session()
        record.sessionMs = Self.ms(since: sessionStarted)
        let cookie = await client.sessionCookie
        record.sessionReused = stored != nil && cookie == stored?.cookie
        if let cookie, cookie != stored?.cookie {
            // A new session: its lifetime starts now (just after the Gateway made it).
            sessions.save(StoredSession(cookie: cookie, issuedAt: now()))
        }
        let availability: SessionAvailability
        switch session {
        case .failure(let issue):
            return finish(.nothingSent(Self.describe(issue)))
        case .success(nil):
            return finish(.nothingSent("CONTROLS DISABLED"))
        case .success(let flags?):
            availability = flags
        }
        guard command.isAvailable(availability) else { return finish(.nothingSent("NOT AVAILABLE")) }

        let requestStarted = ContinuousClock.now
        let exchange = await client.send(command.control)
        record.requestMs = Self.ms(since: requestStarted)
        if await client.sessionCookie == nil {
            // The Gateway no longer knows the session (401): forget it. No retry.
            sessions.delete()
        }
        let outcome = ControlOutcome(exchange)
        if command.isVolume, outcome.sentSomething { lastVolumeCompleted = now() }
        return finish(outcome)
    }

    private func clientFor(_ credentials: BasicCredentials) throws -> GatewayClient {
        if let cached, cached.credentials == credentials { return cached.client }
        let client = try makeClient(credentials)
        cached = (credentials, client)
        return client
    }

    // MARK: Wording (as in the app's result line)

    static func summary(_ command: ExtensionCommand, _ outcome: ControlOutcome) -> String {
        let label = command.label
        switch outcome {
        case .txVerified:
            return "\(label) TX VERIFIED · \(command.effect) UNVERIFIED"
        case .nothingSent(let reason):
            return "\(label) \(pretty(reason)) · NOTHING SENT"
        case .uncertain(let reason):
            return "\(label) \(pretty(reason)) · UNCERTAIN · CHECK RECEIVER"
        case .fault(let result):
            return result == "CD_FAULT_LATCHED" ? "CD FAULT LATCHED · STOP AND INSPECT CHANGER"
                : "\(label) FAULT LATCHED · CHECK RECEIVER · RESTART REQUIRED"
        }
    }

    static func describe(_ issue: GatewayIssue) -> String {
        switch issue {
        case .authenticationRejected: "PASSWORD REJECTED"
        case .transport(let error): error.resultName
        case .gatewayUnavailable(let code): "GATEWAY UNAVAILABLE HTTP \(code)"
        case .hostMismatch: "HOST REJECTED"
        case .unexpectedResponse(let code): "SESSION HTTP \(code)"
        case .invalidBody: "UNREADABLE SESSION"
        }
    }

    private static func pretty(_ result: String) -> String { result.replacingOccurrences(of: "_", with: " ") }

    private static func ms(since start: ContinuousClock.Instant) -> Int {
        let elapsed = ContinuousClock.now - start
        return Int(elapsed.components.seconds * 1000 + elapsed.components.attoseconds / 1_000_000_000_000_000)
    }
}
