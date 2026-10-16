import AppIntents
import Darwin
import Foundation
import Synchronization

/// Reads `GET /api/v1/status` through the app's own Gateway client: pinned
/// private CA, Basic Auth from the shared keychain, no Gateway session, no XS8
/// transmission. Requires an unlocked device, as planned for every hardware
/// action in v1.
struct GatewayStatusIntent: AppIntent {
    static let title: LocalizedStringResource = "Kenwood Gateway Status"
    static let description = IntentDescription("Reads the Gateway status. Sends nothing to the receiver.")
    static let authenticationPolicy: IntentAuthenticationPolicy = .requiresAuthentication
    static let isDiscoverable = false

    func perform() async throws -> some IntentResult & ProvidesDialog {
        let diagnostic = await StatusDiagnostic.run()
        _ = diagnostic.save()
        guard diagnostic.ok else { throw DiagnosticFailure(message: diagnostic.summary) }
        return .result(dialog: IntentDialog(stringLiteral: diagnostic.summary))
    }
}

/// Shown by the system if it presents intent errors for controls (a spike question).
struct DiagnosticFailure: Error, CustomLocalizedStringResourceConvertible {
    let message: String
    var localizedStringResource: LocalizedStringResource { "\(message)" }
}

enum StatusDiagnostic {
    private static let invocations = Mutex(0)

    static func run() async -> ControlDiagnostic {
        let started = ContinuousClock.now
        let invocation = invocations.withLock { count in
            count += 1
            return count
        }
        var record = ControlDiagnostic(date: Date(), processID: getpid(), processAgeMs: processAgeMs(),
                                       invocationInProcess: invocation, keychainMs: nil, requestMs: nil,
                                       totalMs: 0, ok: false, summary: "", command: "STATUS")
        func finish(_ ok: Bool, _ summary: String) -> ControlDiagnostic {
            record.ok = ok
            record.summary = summary
            record.totalMs = milliseconds(since: started)
            return record
        }

        let configuration = GatewayConfiguration.woodland
        let keychainStarted = ContinuousClock.now
        let credentials: BasicCredentials
        do {
            let store = KeychainCredentialStore(server: configuration.host,
                                                accessGroup: GatewayConfiguration.sharedKeychainGroup)
            guard let loaded = try store.load() else {
                return finish(false, "NO SHARED CREDENTIALS · OPEN THE APP ONCE")
            }
            credentials = loaded
        } catch let error as KeychainError {
            return finish(false, error.isLocked ? "DEVICE LOCKED · CREDENTIALS UNAVAILABLE"
                                                : "KEYCHAIN ERROR \(error.status)")
        } catch {
            return finish(false, "KEYCHAIN ERROR")
        }
        record.keychainMs = milliseconds(since: keychainStarted)

        let client: GatewayClient
        do {
            client = try await ClientCache.shared.client(for: credentials, configuration: configuration)
        } catch {
            return finish(false, "CA NOT USABLE IN EXTENSION · \(error)")
        }
        let requestStarted = ContinuousClock.now
        let result = await client.status()
        record.requestMs = milliseconds(since: requestStarted)
        switch result {
        case .success(let status):
            guard status.serviceConnected else {
                return finish(true, "GATEWAY OK · XS8 SERVICE UNAVAILABLE · \(record.requestMs ?? 0) ms")
            }
            let source = status.receiver.map { "\($0.source.value) \($0.source.freshness)" } ?? "UNKNOWN"
            return finish(true, "GATEWAY OK · XS8 \(status.status?["state"] ?? "?") · SOURCE \(source) · \(record.requestMs ?? 0) ms")
        case .failure(let issue):
            return finish(false, describe(issue))
        }
    }

    static func describe(_ issue: GatewayIssue) -> String {
        switch issue {
        case .authenticationRejected: "PASSWORD REJECTED"
        case .transport(.tlsRejected(let reason)): "CERTIFICATE REJECTED · \(reason)"
        case .transport(.unreachable(let reason)): "PI NOT REACHED · \(reason)"
        case .transport(let error): "NETWORK · \(error.resultName)"
        case .gatewayUnavailable(let code): "GATEWAY UNAVAILABLE · HTTP \(code)"
        case .hostMismatch: "HOST REJECTED · HTTP 421"
        case .unexpectedResponse(let code): "UNEXPECTED HTTP \(code)"
        case .invalidBody: "UNREADABLE STATUS"
        }
    }

    private static func milliseconds(since start: ContinuousClock.Instant) -> Int {
        let elapsed = ContinuousClock.now - start
        return Int(elapsed.components.seconds * 1000 + elapsed.components.attoseconds / 1_000_000_000_000_000)
    }

    /// Age of this process from the kernel's start time: shows cold starts.
    static func processAgeMs() -> Int? {
        var info = kinfo_proc()
        var size = MemoryLayout<kinfo_proc>.stride
        var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()]
        guard sysctl(&mib, u_int(mib.count), &info, &size, nil, 0) == 0 else { return nil }
        let start = info.kp_proc.p_starttime
        let started = Double(start.tv_sec) + Double(start.tv_usec) / 1_000_000
        return Int((Date().timeIntervalSince1970 - started) * 1000)
    }
}

/// One Gateway client per extension process and credentials: a reused process
/// reuses its TLS connection (and, for later write controls, its session).
actor ClientCache {
    static let shared = ClientCache()
    private var entry: (credentials: BasicCredentials, client: GatewayClient)?

    func client(for credentials: BasicCredentials,
                configuration: GatewayConfiguration) throws -> GatewayClient {
        if let entry, entry.credentials == credentials { return entry.client }
        let anchor = try PinnedCertificateAuthority.load(configuration)
        let transport = URLSessionTransport(
            trust: PinnedTrustEvaluator(host: configuration.host, anchor: anchor),
            credentials: credentials)
        let client = GatewayClient(baseURL: configuration.baseURL, transport: transport)
        entry = (credentials, client)
        return client
    }
}
