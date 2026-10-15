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

/// Presentation state for the remote. It only reads: there is no control path
/// in this build, so nothing here can transmit on the XS8 bus.
@MainActor @Observable
final class RemoteModel {
    typealias ClientFactory = @MainActor (BasicCredentials) throws -> GatewayClient

    static let pollInterval: Duration = .seconds(5)

    private(set) var connection: ConnectionState
    private(set) var snapshot: GatewaySnapshot?
    private(set) var lastSuccess: Date?
    private(set) var savedUsername: String?
    var panel = ContextPanelState()

    private let credentialStore: any CredentialStore
    private let makeClient: ClientFactory
    private var client: GatewayClient?
    private var inFlight: Task<Void, Never>?
    /// Bumped whenever the client is replaced, so a read started with old
    /// credentials can never be applied afterwards.
    private var generation = 0

    init(credentialStore: any CredentialStore, makeClient: @escaping ClientFactory) {
        self.credentialStore = credentialStore
        self.makeClient = makeClient
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
    var sourceIndicator: SourceIndicator { SourceIndicator(receiver: receiver, connected: isOnline) }
    var monitorIndicator: MonitorIndicator { MonitorIndicator(receiver: receiver, connected: isOnline) }
    var powerIndicator: PowerIndicator {
        PowerIndicator(power: snapshot?.status.power, connected: isOnline)
    }
    var capabilities: Capabilities? { isOnline ? snapshot?.capabilities : nil }

    // MARK: Refresh

    /// One read at a time; a second caller (pull-to-refresh during a poll)
    /// waits for the read already running instead of starting another.
    func refresh() async {
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

    private func performRefresh() async {
        guard let client else { return }
        let started = generation
        let result = await client.refresh()
        guard started == generation else { return }
        switch result {
        case .success(let snapshot):
            self.snapshot = snapshot
            lastSuccess = Date()
            connection = snapshot.status.serviceConnected
                ? .online : .serviceUnavailable(snapshot.status.error)
            panel.observe(sourceIndicator.displayedSource)
        case .failure(let issue):
            // Keep the last snapshot for diagnostics only: every indicator
            // requires an online connection, so nothing stale is shown as current.
            connection = ConnectionState(issue)
        }
    }

    // MARK: Credentials

    func saveCredentials(_ credentials: BasicCredentials) throws {
        try credentialStore.save(credentials)
        install(credentials)
    }

    func forgetCredentials() throws {
        try credentialStore.delete()
        generation += 1
        client = nil
        snapshot = nil
        savedUsername = nil
        connection = .needsCredentials
    }

    private func install(_ credentials: BasicCredentials) {
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
    static func live(configuration: GatewayConfiguration = .woodland) -> RemoteModel {
        RemoteModel(credentialStore: KeychainCredentialStore(server: configuration.host)) { credentials in
            let anchor = try PinnedCertificateAuthority.load(configuration)
            let transport = URLSessionTransport(
                trust: PinnedTrustEvaluator(host: configuration.host, anchor: anchor),
                credentials: credentials)
            return GatewayClient(baseURL: configuration.baseURL, transport: transport)
        }
    }
}
