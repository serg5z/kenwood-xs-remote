import Foundation
@testable import KenwoodRemote

/// A scripted Gateway: one queue of replies per path (then an optional default
/// for that path), every request recorded. Nothing leaves the process.
final class StubTransport: HTTPTransport, @unchecked Sendable {
    enum Reply: Sendable {
        case http(Int, Data, [String: String], delayMs: Int = 0)
        case failure(TransportError, delayMs: Int = 0)

        static func json(_ status: Int, _ body: String, delayMs: Int = 0,
                         headers: [String: String] = [:]) -> Reply {
            .http(status, Data(body.utf8),
                  ["Content-Type": "application/json; charset=utf-8"].merging(headers) { $1 },
                  delayMs: delayMs)
        }

        static func html(_ status: Int) -> Reply {
            .http(status, Data("<html>\(status)</html>".utf8), ["Content-Type": "text/html"])
        }
    }

    private let lock = NSLock()
    private var replies: [String: [Reply]] = [:]
    private var defaults: [String: Reply] = [:]
    private var responders: [String: @Sendable (String) -> Reply] = [:]
    private var recorded: [URLRequest] = []

    var requests: [URLRequest] { lock.withLock { recorded } }
    var paths: [String] { requests.map { $0.url!.path } }
    var posts: [URLRequest] { requests.filter { $0.httpMethod == "POST" } }
    var postBodies: [String] { posts.map { String(decoding: $0.httpBody ?? Data(), as: UTF8.self) } }

    func posts(to path: String) -> [String] {
        posts.filter { $0.url!.path == path }.map { String(decoding: $0.httpBody ?? Data(), as: UTF8.self) }
    }

    func enqueue(_ endpoint: ReadEndpoint, _ reply: Reply) { enqueue(path: endpoint.rawValue, reply) }

    func enqueue(path: String, _ reply: Reply) {
        lock.withLock { replies[path, default: []].append(reply) }
    }

    /// Answers by request body after the queue is empty (for example by hold action).
    func setResponder(path: String, _ responder: @escaping @Sendable (String) -> Reply) {
        lock.withLock { responders[path] = responder }
    }

    func setDefault(path: String, _ reply: Reply) {
        lock.withLock { defaults[path] = reply }
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

    /// Every later read answers healthily unless something is queued.
    func setHealthyReadDefaults(capabilities: Data = Fixture.data("capabilities"),
                                status: Data = Fixture.data("status-connected")) {
        let json = ["Content-Type": "application/json; charset=utf-8"]
        setDefault(path: ReadEndpoint.status.rawValue, .http(200, status, json))
        setDefault(path: ReadEndpoint.session.rawValue, .http(200, Fixture.data("session"), json))
        setDefault(path: ReadEndpoint.capabilities.rawValue, .http(200, capabilities, json))
    }

    func send(_ request: URLRequest) async throws(TransportError) -> (Data, HTTPURLResponse) {
        let reply: Reply? = lock.withLock {
            recorded.append(request)
            let path = request.url!.path
            if var queue = replies[path], !queue.isEmpty {
                let next = queue.removeFirst()
                replies[path] = queue
                return next
            }
            if let responder = responders[path] {
                return responder(String(decoding: request.httpBody ?? Data(), as: UTF8.self))
            }
            return defaults[path]
        }
        switch reply {
        case .http(let code, let body, let headers, let delayMs):
            if delayMs > 0 {
                do { try await Task.sleep(for: .milliseconds(delayMs)) } catch { throw .cancelled }
            }
            return (body, HTTPURLResponse(url: request.url!, statusCode: code,
                                          httpVersion: "HTTP/1.1", headerFields: headers)!)
        case .failure(let error, let delayMs):
            if delayMs > 0 { try? await Task.sleep(for: .milliseconds(delayMs)) }
            throw error
        case nil:
            throw .other("no stubbed reply for \(request.url!.path)")
        }
    }
}

enum Fixture {
    /// Resolved once: tests run in parallel, and a per-call bundle lookup was
    /// seen to fail intermittently under that load.
    private static let directory: URL = Bundle(for: BundleToken.self).resourceURL!

    static func data(_ name: String) -> Data {
        for ext in ["json", "der"] {
            if let data = try? Data(contentsOf: directory.appending(path: "\(name).\(ext)")) {
                return data
            }
        }
        fatalError("missing fixture \(name) in \(directory.path)")
    }

    /// The capabilities fixture with one operation's fields changed.
    static func capabilities(_ operation: String, _ changes: [String: Any]) -> Data {
        var root = try! JSONSerialization.jsonObject(with: data("capabilities")) as! [String: Any]
        var operations = root["operations"] as! [String: Any]
        var entry = operations[operation] as! [String: Any]
        entry.merge(changes) { $1 }
        operations[operation] = entry
        root["operations"] = operations
        return try! JSONSerialization.data(withJSONObject: root)
    }

    /// The connected status fixture with service STATUS fields changed.
    static func status(_ changes: [String: String]) -> Data {
        var root = try! JSONSerialization.jsonObject(with: data("status-connected")) as! [String: Any]
        var status = root["status"] as! [String: String]
        status.merge(changes) { $1 }
        root["status"] = status
        return try! JSONSerialization.data(withJSONObject: root)
    }

    private final class BundleToken {}
}
