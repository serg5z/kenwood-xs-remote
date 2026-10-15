import Foundation
import Observation

enum ConnectionState: Equatable, Sendable {
    case needsCredentials
    case connecting
    /// Gateway reachable and the XS8 Service connected.
    case online
    /// The Gateway answered, but the XS8 Service did not (STATUS 503).
    case serviceUnavailable(String?)
    case authenticationRejected
    case tlsRejected(String)
    case unreachable(String)
    case failed(String)
    /// The build cannot connect at all (for example, the CA is missing).
    case configurationError(String)

    var isOnline: Bool { self == .online }

    var title: String {
        switch self {
        case .needsCredentials: "Sign in required"
        case .connecting: "Connecting…"
        case .online: "Connected"
        case .serviceUnavailable: "XS8 Service unavailable"
        case .authenticationRejected: "Password rejected"
        case .tlsRejected: "Certificate rejected"
        case .unreachable: "Pi unreachable"
        case .failed: "Connection problem"
        case .configurationError: "Build configuration error"
        }
    }

    var detail: String? {
        switch self {
        case .serviceUnavailable(let reason): reason
        case .tlsRejected(let reason), .unreachable(let reason), .failed(let reason),
             .configurationError(let reason): reason
        default: nil
        }
    }

    init(_ issue: GatewayIssue) {
        switch issue {
        case .authenticationRejected: self = .authenticationRejected
        case .transport(.tlsRejected(let reason)): self = .tlsRejected(reason)
        case .transport(.unreachable(let reason)): self = .unreachable(reason)
        case .transport(.timedOut): self = .failed("Request timed out")
        case .transport(.connectionLost): self = .failed("Connection lost")
        case .transport(.cancelled): self = .failed("Request cancelled")
        case .transport(.other(let reason)): self = .failed(reason)
        case .gatewayUnavailable(let code): self = .failed("Gateway unavailable (HTTP \(code))")
        case .hostMismatch: self = .failed("nginx rejected the host name (HTTP 421)")
        case .unexpectedResponse(let code): self = .failed("Unexpected HTTP \(code)")
        case .invalidBody(let endpoint): self = .failed("Unreadable reply from \(endpoint.rawValue)")
        }
    }
}

/// The one-line result of the last control, worded like the Web Remote.
struct ResultLine: Equatable, Sendable {
    enum Tone: Equatable, Sendable { case progress, verified, neutral, nothingSent, uncertain, fault }
    let text: String
    let tone: Tone
}

/// A latched fault reported by a control; controls of that kind stay off until
/// the next successful read shows the Gateway's own view.
enum FaultScope: Equatable, Sendable { case cd, service }

/// Presentation and interaction state. Every receiver indication comes from
/// Gateway observations; control results only produce the result line.
@MainActor @Observable
final class RemoteModel {
    typealias ClientFactory = @MainActor (BasicCredentials) throws -> GatewayClient
    /// Starts an OS background task so a RELEASE can still go out after the app
    /// leaves the foreground; returns the function that ends it.
    typealias BackgroundTaskStarter = @MainActor (String) -> (@MainActor () -> Void)

    static let pollInterval: Duration = .seconds(5)

    private(set) var connection: ConnectionState
    private(set) var snapshot: GatewaySnapshot?
    private(set) var lastSuccess: Date?
    private(set) var savedUsername: String?
    var panel = ContextPanelState()

    // Control state (written by RemoteModel+Controls)
    /// True while a control or hold is in flight. Taps meanwhile are dropped,
    /// never queued.
    var busy = false
    var lastResult: ResultLine?
    var activeHold: ActiveHold?
    /// A volume key is down and its tap/hold decision is pending.
    var pendingPress: HoldKey?
    /// The explicit ON / OFF choice (no TOGGLE, or after a refused one).
    var showingPowerChooser = false
    var localFault: FaultScope?
    var sourceSince: UInt64?
    var monitorSince: UInt64?

    @ObservationIgnored let now: @MainActor () -> Date
    @ObservationIgnored let beginBackgroundTask: BackgroundTaskStarter
    @ObservationIgnored var spacing = ControlSpacing()
    @ObservationIgnored var pressTimer: Task<Void, Never>?
    @ObservationIgnored var controlTask: Task<Void, Never>?
    @ObservationIgnored var holdTask: Task<Void, Never>?
    @ObservationIgnored private(set) var client: GatewayClient?

    @ObservationIgnored private let credentialStore: any CredentialStore
    @ObservationIgnored private let makeClient: ClientFactory
    @ObservationIgnored private var inFlight: Task<Void, Never>?
    /// Bumped whenever the client is replaced, so a read started with old
    /// credentials can never be applied afterwards.
    @ObservationIgnored private var generation = 0

    init(credentialStore: any CredentialStore,
         now: @escaping @MainActor () -> Date = { Date() },
         beginBackgroundTask: @escaping BackgroundTaskStarter = { _ in {} },
         makeClient: @escaping ClientFactory) {
        self.credentialStore = credentialStore
        self.makeClient = makeClient
        self.now = now
        self.beginBackgroundTask = beginBackgroundTask
        connection = .needsCredentials
        do {
            if let credentials = try credentialStore.load() { install(credentials) }
        } catch {
            connection = .configurationError("Keychain read failed (\(error))")
        }
    }

    // MARK: Derived observations

    var isOnline: Bool { connection.isOnline }
    var receiver: ReceiverObservations? { snapshot?.status.receiver }
    var sourceIndicator: SourceIndicator {
        SourceIndicator(receiver: receiver, connected: isOnline, since: sourceSince)
    }
    var monitorIndicator: MonitorIndicator {
        MonitorIndicator(receiver: receiver, connected: isOnline, since: monitorSince)
    }
    var powerIndicator: PowerIndicator {
        PowerIndicator(power: snapshot?.status.power, connected: isOnline)
    }
    var capabilities: Capabilities? { isOnline ? snapshot?.capabilities : nil }

    /// A fault the XS8 Service reports in STATUS. The Gateway refuses the
    /// affected controls; this only explains why.
    var faultBanner: String? {
        guard isOnline, let status = snapshot?.status.status else { return nil }
        if status["state"] == "FAULT_LATCHED" { return "XS8 FAULT LATCHED · CHECK RECEIVER · RESTART REQUIRED" }
        if status["cd_state"] == "FAULT_LATCHED" ||
            (status["cd_state"] == "IDLE" && status["cd_release_owed"] == "1") {
            return "CD FAULT LATCHED · STOP AND INSPECT CHANGER"
        }
        if let monitor = status["monitor"], monitor != "HEALTHY" {
            return "XS8 MONITOR \(monitor) · CONTROLS REFUSED"
        }
        return nil
    }

    // MARK: Refresh

    /// One read at a time; a second caller (pull-to-refresh during a poll)
    /// waits for the read already running. No reads while a control runs: the
    /// control refreshes when it finishes.
    func refresh() async {
        guard !busy else { return }
        await refreshNow()
    }

    func refreshNow() async {
        if let inFlight {
            await inFlight.value
            return
        }
        let task = Task { await self.performRefresh() }
        inFlight = task
        await task.value
        if inFlight == task { inFlight = nil }
    }

    /// Polls while the calling task lives; the view cancels it when the app
    /// leaves the foreground.
    func pollWhileActive() async {
        while !Task.isCancelled {
            await refresh()
            do { try await Task.sleep(for: Self.pollInterval) } catch { return }
        }
    }

    /// Ends a running read before a control, so the control does not wait for it.
    func cancelRefresh() async {
        guard let task = inFlight else { return }
        task.cancel()
        await task.value
    }

    private func performRefresh() async {
        guard let client else { return }
        let started = generation
        let result = await client.refresh()
        // A cancelled read (a control started) is dropped, not shown as a failure.
        guard started == generation, !Task.isCancelled else { return }
        switch result {
        case .success(let snapshot):
            self.snapshot = snapshot
            lastSuccess = now()
            connection = snapshot.status.serviceConnected
                ? .online : .serviceUnavailable(snapshot.status.error)
            localFault = nil
            if !connection.isOnline { clearObservationGuards() }
            panel.observe(sourceIndicator.displayedSource)
        case .failure(let issue):
            // Keep the last snapshot for diagnostics only: every indicator
            // requires an online connection, so nothing stale is shown as current.
            connection = ConnectionState(issue)
            // A restarted Pi restarts its monotonic clock.
            clearObservationGuards()
        }
    }

    private func clearObservationGuards() {
        sourceSince = nil
        monitorSince = nil
    }

    // MARK: Credentials

    func saveCredentials(_ credentials: BasicCredentials) throws {
        try credentialStore.save(credentials)
        install(credentials)
    }

    func forgetCredentials() throws {
        try credentialStore.delete()
        stopInteraction(reason: "SIGNED OUT")
        generation += 1
        client = nil
        snapshot = nil
        savedUsername = nil
        connection = .needsCredentials
    }

    private func install(_ credentials: BasicCredentials) {
        stopInteraction(reason: "ACCOUNT CHANGED")
        generation += 1
        snapshot = nil
        savedUsername = credentials.username
        do {
            client = try makeClient(credentials)
            connection = .connecting
        } catch {
            client = nil
            connection = .configurationError(Self.describe(error))
        }
    }

    private static func describe(_ error: Error) -> String {
        switch error {
        case CertificateAuthorityError.missing(let name):
            "\(name) is not in the app bundle; see ios/KenwoodRemote/README.md"
        case CertificateAuthorityError.fingerprintMismatch(let digest):
            "Bundled CA fingerprint \(digest.prefix(16))… does not match the configured CA"
        case CertificateAuthorityError.invalid:
            "Bundled CA is not a valid DER certificate"
        default:
            "\(error)"
        }
    }
}

extension RemoteModel {
    static func live(configuration: GatewayConfiguration = .woodland,
                     beginBackgroundTask: @escaping BackgroundTaskStarter) -> RemoteModel {
        RemoteModel(credentialStore: KeychainCredentialStore(server: configuration.host),
                    beginBackgroundTask: beginBackgroundTask) { credentials in
            let anchor = try PinnedCertificateAuthority.load(configuration)
            let transport = URLSessionTransport(
                trust: PinnedTrustEvaluator(host: configuration.host, anchor: anchor),
                credentials: credentials)
            return GatewayClient(baseURL: configuration.baseURL, transport: transport)
        }
    }
}
