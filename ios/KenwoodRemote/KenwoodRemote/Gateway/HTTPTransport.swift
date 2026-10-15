import Foundation

/// The network boundary. Production uses URLSession; tests use a stub.
protocol HTTPTransport: Sendable {
    func send(_ request: URLRequest) async throws(TransportError) -> (Data, HTTPURLResponse)
}

/// A request that produced no HTTP response. `mayHaveReachedServer` is the
/// distinction controls will need: only a failure known to precede any
/// request bytes is "nothing sent"; everything else is uncertain.
enum TransportError: Error, Equatable, Sendable {
    /// Our pinned-CA evaluation rejected the server, or the OS reported a
    /// certificate failure. The TLS handshake never completed.
    case tlsRejected(String)
    /// Name resolution or connection failed before any request was sent.
    case unreachable(String)
    case timedOut
    case connectionLost
    case cancelled
    case other(String)

    var mayHaveReachedServer: Bool {
        switch self {
        case .tlsRejected, .unreachable: false
        case .timedOut, .connectionLost, .cancelled, .other: true
        }
    }

    /// Conservative mapping: an unlisted error counts as possibly delivered.
    /// `trustFailure` is our own delegate's rejection, which surfaces as a
    /// cancelled task.
    init(_ error: URLError, trustFailure: TrustFailure?) {
        if let trustFailure {
            self = .tlsRejected(trustFailure.message)
            return
        }
        switch error.code {
        case .serverCertificateUntrusted, .serverCertificateHasBadDate,
             .serverCertificateHasUnknownRoot, .serverCertificateNotYetValid,
             .clientCertificateRejected, .clientCertificateRequired,
             .appTransportSecurityRequiresSecureConnection:
            self = .tlsRejected(error.localizedDescription)
        case .cannotFindHost, .dnsLookupFailed, .cannotConnectToHost,
             .notConnectedToInternet, .internationalRoamingOff, .callIsActive,
             .dataNotAllowed:
            self = .unreachable(error.localizedDescription)
        case .timedOut:
            self = .timedOut
        case .networkConnectionLost:
            self = .connectionLost
        case .cancelled:
            self = .cancelled
        default:
            // Includes secureConnectionFailed: not provably before the request.
            self = .other("\(error.code.rawValue) \(error.localizedDescription)")
        }
    }
}

struct BasicCredentials: Sendable, Equatable {
    let username: String
    let password: String
}

/// URLSession with no shared state: an ephemeral configuration, no cookie
/// storage (the client manages the one session cookie), no cache, and no
/// waiting for connectivity, so a request never goes out late.
final class URLSessionTransport: HTTPTransport {
    private let session: URLSession
    private let delegate: GatewaySessionDelegate

    init(trust: PinnedTrustEvaluator, credentials: BasicCredentials) {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.httpShouldSetCookies = false
        configuration.httpCookieAcceptPolicy = .never
        configuration.httpCookieStorage = nil
        configuration.urlCache = nil
        configuration.requestCachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        configuration.waitsForConnectivity = false
        configuration.httpMaximumConnectionsPerHost = 1
        delegate = GatewaySessionDelegate(trust: trust, credentials: credentials)
        session = URLSession(configuration: configuration, delegate: delegate, delegateQueue: nil)
    }

    deinit { session.invalidateAndCancel() }

    func send(_ request: URLRequest) async throws(TransportError) -> (Data, HTTPURLResponse) {
        delegate.clearTrustFailure()
        do {
            let (data, response) = try await session.data(for: request)
            guard let http = response as? HTTPURLResponse else {
                throw TransportError.other("Not an HTTP response")
            }
            return (data, http)
        } catch let error as TransportError {
            throw error
        } catch let error as URLError {
            throw TransportError(error, trustFailure: delegate.trustFailure)
        } catch {
            throw TransportError.other(error.localizedDescription)
        }
    }
}

/// Answers the two challenges the Gateway route produces: server trust (only
/// the pinned CA) and nginx Basic Auth (only over a verified HTTPS connection
/// to the Gateway host).
final class GatewaySessionDelegate: NSObject, URLSessionTaskDelegate, @unchecked Sendable {
    private let trust: PinnedTrustEvaluator
    private let credentials: BasicCredentials
    private let lock = NSLock()
    private var lastTrustFailure: TrustFailure?

    init(trust: PinnedTrustEvaluator, credentials: BasicCredentials) {
        self.trust = trust
        self.credentials = credentials
    }

    var trustFailure: TrustFailure? { lock.withLock { lastTrustFailure } }
    func clearTrustFailure() { lock.withLock { lastTrustFailure = nil } }

    func urlSession(_ session: URLSession, didReceive challenge: URLAuthenticationChallenge,
                    completionHandler: @escaping @Sendable (URLSession.AuthChallengeDisposition,
                                                            URLCredential?) -> Void) {
        let (disposition, credential) = handle(challenge)
        completionHandler(disposition, credential)
    }

    func urlSession(_ session: URLSession, task: URLSessionTask,
                    didReceive challenge: URLAuthenticationChallenge,
                    completionHandler: @escaping @Sendable (URLSession.AuthChallengeDisposition,
                                                            URLCredential?) -> Void) {
        let (disposition, credential) = handle(challenge)
        completionHandler(disposition, credential)
    }

    private func handle(_ challenge: URLAuthenticationChallenge)
        -> (URLSession.AuthChallengeDisposition, URLCredential?) {
        let space = challenge.protectionSpace
        switch space.authenticationMethod {
        case NSURLAuthenticationMethodServerTrust:
            guard let serverTrust = space.serverTrust else {
                lock.withLock { lastTrustFailure = .rejected("no server trust") }
                return (.cancelAuthenticationChallenge, nil)
            }
            if let failure = trust.evaluate(serverTrust, host: space.host) {
                lock.withLock { lastTrustFailure = failure }
                return (.cancelAuthenticationChallenge, nil)
            }
            return (.useCredential, URLCredential(trust: serverTrust))
        case NSURLAuthenticationMethodHTTPBasic:
            // Never answer for another host or over an unverified channel. A
            // repeated challenge means the password was refused: let nginx's 401
            // through instead of looping.
            guard space.host == trust.host, space.receivesCredentialSecurely,
                  challenge.previousFailureCount == 0 else {
                return (.rejectProtectionSpace, nil)
            }
            return (.useCredential, URLCredential(user: credentials.username,
                                                  password: credentials.password,
                                                  persistence: .forSession))
        default:
            return (.rejectProtectionSpace, nil)
        }
    }
}
