import Foundation

/// The Gateway's read endpoints. This client has no write path: every request it
/// can build is a body-less GET, which the Gateway answers without transmitting
/// anything on the XS8 bus.
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

actor GatewayClient {
    static let readTimeout: TimeInterval = 4

    private let baseURL: URL
    private let transport: any HTTPTransport
    /// The Gateway's `xs8_session` cookie, managed by hand: exactly one Cookie
    /// header, never persisted (sessions last 30 min and die with the Gateway).
    private(set) var sessionCookie: String?

    init(baseURL: URL, transport: any HTTPTransport) {
        self.baseURL = baseURL
        self.transport = transport
    }

    func refresh() async -> Result<GatewaySnapshot, GatewayIssue> {
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

    func fetchStatus() async throws(GatewayIssue) -> StatusResponse {
        let (data, response) = try await get(.status)
        // 503 is a well-formed Gateway reply: the Gateway is up, the service is not.
        guard response.statusCode == 200 || (response.statusCode == 503 && isJSON(response)) else {
            throw classify(response)
        }
        return try decode(StatusResponse.self, data, .status)
    }

    /// nil when the Gateway has every control gate disabled (404).
    func fetchSession() async throws(GatewayIssue) -> SessionAvailability? {
        let (data, response) = try await get(.session)
        if response.statusCode == 404 && isJSON(response) { return nil }
        guard response.statusCode == 200 else { throw classify(response) }
        if let cookie = Self.sessionCookie(from: response) { sessionCookie = cookie }
        return try decode(SessionAvailability.self, data, .session)
    }

    func fetchCapabilities() async throws(GatewayIssue) -> Capabilities {
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
