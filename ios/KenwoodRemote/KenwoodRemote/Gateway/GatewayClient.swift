import Foundation

/// The Gateway's read endpoints. GETs never transmit on the XS8 bus.
enum ReadEndpoint: String, CaseIterable, Sendable {
    case status = "/api/v1/status"
    case session = "/api/v1/session"
    case capabilities = "/api/v1/capabilities"
}

/// Why a read produced no usable result.
enum GatewayIssue: Error, Equatable, Sendable {
    case transport(TransportError)
    /// nginx refused the Basic Auth credentials (401 with WWW-Authenticate).
    case authenticationRejected
    /// nginx could not reach the Gateway socket, or timed out (non-JSON 502/504).
    case gatewayUnavailable(Int)
    /// nginx answered 421: the Host was not the configured name.
    case hostMismatch
    case unexpectedResponse(Int)
    case invalidBody(ReadEndpoint)
}

/// One coherent read: STATUS always, session availability and capabilities when
/// the XS8 Service is connected. Applied together by the model.
struct GatewaySnapshot: Sendable, Equatable {
    let status: StatusResponse
    /// nil with every control gate off (`/session` 404), or when not fetched.
    let session: SessionAvailability?
    let capabilities: Capabilities?
    /// Why session/capabilities are missing even though STATUS succeeded.
    let controlInfoIssue: GatewayIssue?
    let controlsConfigured: Bool
}

/// All Gateway traffic goes through one serial lane: a read and a control
/// never interleave, so a session GET can never hand back a CSRF token that a
/// control has already rotated, and controls run strictly one at a time.
actor GatewayClient {
    static let readTimeout: TimeInterval = 4

    private let baseURL: URL
    /// The exact Origin the Gateway requires (`--https-origin`).
    let origin: String
    private let transport: any HTTPTransport
    /// The Gateway's `xs8_session` cookie, managed by hand: exactly one Cookie
    /// header, never persisted (sessions last 30 min and die with the Gateway).
    private(set) var sessionCookie: String?
    /// The session's current one-use CSRF token: from `GET /session` or the
    /// successor in a control reply. Taken (cleared) by every control request.
    private var csrfToken: String?

    private var laneBusy = false
    private var laneWaiters: [CheckedContinuation<Void, Never>] = []

    init(baseURL: URL, transport: any HTTPTransport) {
        self.baseURL = baseURL
        origin = "\(baseURL.scheme ?? "https")://\(baseURL.host() ?? "")"
            + (baseURL.port.map { ":\($0)" } ?? "")
        self.transport = transport
    }

    // MARK: Serial lane

    private func enterLane() async {
        guard laneBusy else { laneBusy = true; return }
        await withCheckedContinuation { laneWaiters.append($0) }
    }

    private func leaveLane() {
        if laneWaiters.isEmpty { laneBusy = false } else { laneWaiters.removeFirst().resume() }
    }

    // MARK: Reads

    func refresh() async -> Result<GatewaySnapshot, GatewayIssue> {
        await enterLane()
        defer { leaveLane() }
        let status: StatusResponse
        do {
            status = try await fetchStatus()
        } catch {
            return .failure(error)
        }
        guard status.serviceConnected else {
            return .success(GatewaySnapshot(status: status, session: nil, capabilities: nil,
                                            controlInfoIssue: nil, controlsConfigured: false))
        }
        // The session comes first: capabilities report `enabled` only for a
        // request carrying a valid session cookie.
        let session: SessionAvailability?
        do {
            session = try await fetchSession()
        } catch {
            return .success(GatewaySnapshot(status: status, session: nil, capabilities: nil,
                                            controlInfoIssue: error, controlsConfigured: false))
        }
        guard session != nil else {
            return .success(GatewaySnapshot(status: status, session: nil, capabilities: nil,
                                            controlInfoIssue: nil, controlsConfigured: false))
        }
        do {
            let capabilities = try await fetchCapabilities()
            return .success(GatewaySnapshot(status: status, session: session,
                                            capabilities: capabilities, controlInfoIssue: nil,
                                            controlsConfigured: true))
        } catch {
            return .success(GatewaySnapshot(status: status, session: session, capabilities: nil,
                                            controlInfoIssue: error, controlsConfigured: true))
        }
    }

    /// `GET /api/v1/status` alone. It needs no Gateway session (none is
    /// created) and transmits nothing on the XS8 bus.
    func status() async -> Result<StatusResponse, GatewayIssue> {
        await enterLane()
        defer { leaveLane() }
        do {
            return .success(try await fetchStatus())
        } catch {
            return .failure(error)
        }
    }

    /// `GET /api/v1/session`: confirms the session cookie (the Gateway issues a
    /// new one if it no longer knows it) and takes the session's current CSRF
    /// token and availability. A read; nil when every control gate is off.
    func session() async -> Result<SessionAvailability?, GatewayIssue> {
        await enterLane()
        defer { leaveLane() }
        do {
            return .success(try await fetchSession())
        } catch {
            return .failure(error)
        }
    }

    /// Replaces the session cookie (or drops it with nil) and forgets any token
    /// that belonged to the previous one.
    func useSessionCookie(_ cookie: String?) async {
        await enterLane()
        defer { leaveLane() }
        if cookie != sessionCookie { csrfToken = nil }
        sessionCookie = cookie
    }

    private func fetchStatus() async throws(GatewayIssue) -> StatusResponse {
        let (data, response) = try await get(.status)
        // 503 is a well-formed Gateway reply: the Gateway is up, the service is not.
        guard response.statusCode == 200 || (response.statusCode == 503 && isJSON(response)) else {
            throw classify(response)
        }
        return try decode(StatusResponse.self, data, .status)
    }

    /// nil when the Gateway has every control gate disabled (404). Also stores
    /// the session's current CSRF token.
    private func fetchSession() async throws(GatewayIssue) -> SessionAvailability? {
        let (data, response) = try await get(.session)
        if response.statusCode == 404 && isJSON(response) {
            csrfToken = nil
            return nil
        }
        guard response.statusCode == 200 else { throw classify(response) }
        if let cookie = Self.sessionCookie(from: response) { sessionCookie = cookie }
        let session = try decode(SessionResponse.self, data, .session)
        csrfToken = session.csrfToken
        return session.availability
    }

    private func fetchCapabilities() async throws(GatewayIssue) -> Capabilities {
        let (data, response) = try await get(.capabilities)
        guard response.statusCode == 200 else { throw classify(response) }
        return try decode(Capabilities.self, data, .capabilities)
    }

    func request(for endpoint: ReadEndpoint) -> URLRequest {
        var request = URLRequest(url: URL(string: endpoint.rawValue, relativeTo: baseURL)!.absoluteURL,
                                 cachePolicy: .reloadIgnoringLocalAndRemoteCacheData,
                                 timeoutInterval: Self.readTimeout)
        request.httpMethod = "GET"
        request.setValue("application/json", forHTTPHeaderField: "Accept")
        if let sessionCookie {
            request.setValue("xs8_session=\(sessionCookie)", forHTTPHeaderField: "Cookie")
        }
        return request
    }

    private func get(_ endpoint: ReadEndpoint) async throws(GatewayIssue) -> (Data, HTTPURLResponse) {
        do {
            return try await transport.send(request(for: endpoint))
        } catch {
            throw .transport(error)
        }
    }

    // MARK: Controls

    /// One control request, sent at most once and never retried. `sendGuard` is
    /// checked after this request's turn in the lane and its token are ready,
    /// immediately before transmission.
    func send(_ command: ControlCommand, guard sendGuard: SendGuard? = nil) async -> ControlExchange {
        await enterLane()
        defer { leaveLane() }
        if sendGuard?.isCancelled == true { return .notAttempted("CANCELLED BEFORE SEND") }
        guard let body = command.body else { return .notAttempted("INVALID REQUEST") }
        if csrfToken == nil {
            // After a control without a successor token (or none yet): the
            // session's current token. A read only; it never rotates the token.
            do {
                _ = try await fetchSession()
            } catch {
                return .notAttempted("NO SESSION TOKEN")
            }
        }
        guard let token = csrfToken else { return .notAttempted("NO SESSION TOKEN") }
        if sendGuard?.isCancelled == true { return .notAttempted("CANCELLED BEFORE SEND") }
        // Single use, even before the reply: whatever happens, never reuse it.
        csrfToken = nil

        var request = URLRequest(url: URL(string: command.path, relativeTo: baseURL)!.absoluteURL,
                                 cachePolicy: .reloadIgnoringLocalAndRemoteCacheData,
                                 timeoutInterval: command.timeout)
        request.httpMethod = "POST"
        request.httpBody = body
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.setValue("application/json", forHTTPHeaderField: "Accept")
        request.setValue(origin, forHTTPHeaderField: "Origin")
        request.setValue(token, forHTTPHeaderField: "X-CSRF-Token")
        if let sessionCookie {
            request.setValue("xs8_session=\(sessionCookie)", forHTTPHeaderField: "Cookie")
        }

        let data: Data
        let response: HTTPURLResponse
        do {
            (data, response) = try await transport.send(request)
        } catch {
            return .transportFailed(error)
        }
        let json = isJSON(response)
        let reply = json ? try? JSONDecoder().decode(ControlReply.self, from: data) : nil
        if let successor = reply?.csrfToken { csrfToken = successor }
        if response.statusCode == 401 && json {
            // The Gateway no longer knows this session (expired or restarted).
            sessionCookie = nil
        }
        return .response(status: response.statusCode, reply: reply, gatewayJSON: json)
    }

    // MARK: Helpers

    private func decode<T: Decodable>(_ type: T.Type, _ data: Data,
                                      _ endpoint: ReadEndpoint) throws(GatewayIssue) -> T {
        do {
            return try JSONDecoder().decode(type, from: data)
        } catch {
            throw .invalidBody(endpoint)
        }
    }

    private func isJSON(_ response: HTTPURLResponse) -> Bool {
        response.value(forHTTPHeaderField: "Content-Type")?.hasPrefix("application/json") == true
    }

    private func classify(_ response: HTTPURLResponse) -> GatewayIssue {
        switch response.statusCode {
        case 401 where response.value(forHTTPHeaderField: "WWW-Authenticate") != nil:
            .authenticationRejected
        case 421:
            .hostMismatch
        case let code where [502, 503, 504].contains(code) && !isJSON(response):
            .gatewayUnavailable(code)
        default:
            .unexpectedResponse(response.statusCode)
        }
    }

    /// The `xs8_session` value from Set-Cookie (URLSession may join several
    /// headers with commas). The Gateway issues `secrets.token_urlsafe(32)`.
    static func sessionCookie(from response: HTTPURLResponse) -> String? {
        guard let header = response.value(forHTTPHeaderField: "Set-Cookie") else { return nil }
        let pattern = /(?:^|[;,]\s*)xs8_session=([A-Za-z0-9_-]{16,128})(?=;|,|\s|$)/
        return header.firstMatch(of: pattern).map { String($0.1) }
    }
}
