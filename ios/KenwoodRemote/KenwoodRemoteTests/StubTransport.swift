import Foundation
@testable import KenwoodRemote

/// A scripted Gateway: one queue of replies per path, every request recorded.
final class StubTransport: HTTPTransport, @unchecked Sendable {
    enum Reply {
        case http(Int, Data, [String: String])
        case failure(TransportError)
    }

    private let lock = NSLock()
    private var replies: [String: [Reply]] = [:]
    private var recorded: [URLRequest] = []

    var requests: [URLRequest] { lock.withLock { recorded } }
    var paths: [String] { requests.map { $0.url!.path } }

    func enqueue(_ endpoint: ReadEndpoint, _ reply: Reply) {
        lock.withLock { replies[endpoint.rawValue, default: []].append(reply) }
    }

    func enqueueJSON(_ endpoint: ReadEndpoint, status: Int = 200, fixture: String,
                     headers: [String: String] = [:]) {
        enqueue(endpoint, .http(status, Fixture.data(fixture),
                                ["Content-Type": "application/json; charset=utf-8"]
                                    .merging(headers) { $1 }))
    }

    /// The usual healthy read: connected status, session (with a cookie), capabilities.
    func enqueueHealthyRefresh(cookie: String? = "c00kie-c00kie-c00kie-c00kie") {
        enqueueJSON(.status, fixture: "status-connected")
        enqueueJSON(.session, fixture: "session", headers: cookie.map {
            ["Set-Cookie": "xs8_session=\($0); Path=/; Secure; HttpOnly; SameSite=Strict"]
        } ?? [:])
        enqueueJSON(.capabilities, fixture: "capabilities")
    }

    func send(_ request: URLRequest) async throws(TransportError) -> (Data, HTTPURLResponse) {
        let reply: Reply? = lock.withLock {
            recorded.append(request)
            let path = request.url!.path
            guard var queue = replies[path], !queue.isEmpty else { return nil }
            let next = queue.removeFirst()
            replies[path] = queue
            return next
        }
        switch reply {
        case .http(let code, let body, let headers):
            return (body, HTTPURLResponse(url: request.url!, statusCode: code,
                                          httpVersion: "HTTP/1.1", headerFields: headers)!)
        case .failure(let error):
            throw error
        case nil:
            throw .other("no stubbed reply for \(request.url!.path)")
        }
    }
}

enum Fixture {
    static func data(_ name: String) -> Data {
        let bundle = Bundle(for: BundleToken.self)
        for ext in ["json", "der"] {
            if let url = bundle.url(forResource: name, withExtension: ext) {
                return try! Data(contentsOf: url)
            }
        }
        fatalError("missing fixture \(name)")
    }

    private final class BundleToken {}
}
