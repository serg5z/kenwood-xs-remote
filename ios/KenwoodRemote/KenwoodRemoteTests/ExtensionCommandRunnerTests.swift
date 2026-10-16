import Foundation
import Security
import Testing
@testable import KenwoodRemote

final class MemorySessionStore: SessionCookieStore, @unchecked Sendable {
    private let lock = NSLock()
    private var stored: StoredSession?
    private(set) var saves = 0
    init(_ session: StoredSession? = nil) { stored = session }
    func load() -> StoredSession? { lock.withLock { stored } }
    func save(_ session: StoredSession) { lock.withLock { stored = session; saves += 1 } }
    func delete() { lock.withLock { stored = nil } }
}

final class SendableClock: @unchecked Sendable {
    private let lock = NSLock()
    private var current = Date(timeIntervalSince1970: 1_800_000_000)
    var now: Date { lock.withLock { current } }
    func advance(seconds: TimeInterval) { lock.withLock { current += seconds } }
}

/// Control Center MUTE and volume steps against a scripted Gateway.
@Suite struct ExtensionCommandRunnerTests {
    static let credentials = BasicCredentials(username: "remote", password: "secret")
    static let ok = #"{"result":"SERVICE_TX_OK","receiver_execution":"UNVERIFIED"}"#
    static let cookieA = "extension-session-A-0123456789"
    static let cookieB = "extension-session-B-0123456789"

    let transport = StubTransport()
    let sessions = MemorySessionStore()
    let clock = SendableClock()

    func runner(credentials: @escaping ExtensionCommandRunner.CredentialLoader = { Self.credentials },
                sessions: (any SessionCookieStore)? = nil) -> ExtensionCommandRunner {
        let transport = transport
        let clock = clock
        return ExtensionCommandRunner(loadCredentials: credentials, sessions: sessions ?? self.sessions,
                                      now: { clock.now }) { _ in
            GatewayClient(baseURL: URL(string: "https://woodland-pi.local")!, transport: transport)
        }
    }

    /// `/session`, optionally issuing a cookie, with a token and availability.
    func sessionReply(cookie: String? = nil, token: String = "token-1",
                      mute: Bool = true, volume: Bool = true) {
        transport.enqueue(.session, .json(200,
            #"{"csrf_token":"\#(token)","mute_available":\#(mute),"volume_step_available":\#(volume)}"#,
            headers: cookie.map { ["Set-Cookie": "xs8_session=\($0); Path=/; Secure; HttpOnly; SameSite=Strict"] } ?? [:]))
    }

    // MARK: Requests

    @Test func muteIsOneExactPOSTAfterASessionRead() async throws {
        sessionReply(cookie: Self.cookieA, token: "token-1")
        transport.enqueue(path: "/api/v1/mute", .json(200, Self.ok))
        let record = await runner().run(.mute)

        #expect(transport.paths == ["/api/v1/session", "/api/v1/mute"])
        let post = try #require(transport.posts.first)
        #expect(post.httpBody == Data("{}".utf8))
        #expect(post.value(forHTTPHeaderField: "Origin") == "https://woodland-pi.local")
        #expect(post.value(forHTTPHeaderField: "Content-Type") == "application/json")
        #expect(post.value(forHTTPHeaderField: "X-CSRF-Token") == "token-1")
        #expect(post.value(forHTTPHeaderField: "Cookie") == "xs8_session=\(Self.cookieA)")
        #expect(record.ok)
        #expect(record.command == "MUTE")
        #expect(record.summary == "MUTE TX VERIFIED · RECEIVER ACTION UNVERIFIED")
        #expect(record.sessionReused == false)
        #expect(record.sessionMs != nil && record.requestMs != nil)
    }

    @Test(arguments: [(ExtensionCommand.volumeUp, #"{"direction":"UP"}"#),
                      (.volumeDown, #"{"direction":"DOWN"}"#)])
    func volumeIsOneStep(command: ExtensionCommand, body: String) async {
        sessionReply(cookie: Self.cookieA)
        transport.enqueue(path: "/api/v1/volume/step", .json(200, #"{"result":"SERVICE_TX_OK","csrf_token":"next"}"#))
        let record = await runner().run(command)
        #expect(transport.posts(to: "/api/v1/volume/step") == [body])
        #expect(transport.posts(to: "/api/v1/volume/hold").isEmpty)
        #expect(record.summary == "\(command.label) TX VERIFIED · VOLUME CHANGE UNVERIFIED")
    }

    // MARK: Session cookie

    @Test func aNewSessionIsStoredWithItsIssueTimeAndNoToken() async throws {
        sessionReply(cookie: Self.cookieA)
        transport.enqueue(path: "/api/v1/mute", .json(200, Self.ok))
        _ = await runner().run(.mute)
        let stored = try #require(sessions.load())
        #expect(stored == StoredSession(cookie: Self.cookieA, issuedAt: clock.now))
        let json = try JSONSerialization.jsonObject(with: JSONEncoder().encode(stored)) as! [String: Any]
        #expect(Set(json.keys) == ["cookie", "issuedAt"])   // never a CSRF token
    }

    @Test func aStoredSessionIsReusedAcrossProcesses() async {
        sessions.save(StoredSession(cookie: Self.cookieA, issuedAt: clock.now))
        clock.advance(seconds: 10 * 60)
        sessionReply()                                      // the Gateway accepts it: no Set-Cookie
        transport.enqueue(path: "/api/v1/mute", .json(200, Self.ok))
        let record = await runner().run(.mute)              // a fresh runner = a new extension process
        #expect(transport.requests.allSatisfy { $0.value(forHTTPHeaderField: "Cookie") == "xs8_session=\(Self.cookieA)" })
        #expect(record.sessionReused == true)
        // Reuse never extends the lifetime: the issue time is unchanged.
        #expect(sessions.load()?.issuedAt == Date(timeIntervalSince1970: 1_800_000_000))
    }

    @Test func aSessionOlderThan25MinutesIsNotSent() async {
        sessions.save(StoredSession(cookie: Self.cookieA, issuedAt: clock.now))
        clock.advance(seconds: 25 * 60)
        sessionReply(cookie: Self.cookieB)
        transport.enqueue(path: "/api/v1/mute", .json(200, Self.ok))
        let record = await runner().run(.mute)
        #expect(transport.requests.first?.value(forHTTPHeaderField: "Cookie") == nil)
        #expect(transport.posts.first?.value(forHTTPHeaderField: "Cookie") == "xs8_session=\(Self.cookieB)")
        #expect(sessions.load() == StoredSession(cookie: Self.cookieB, issuedAt: clock.now))
        #expect(record.sessionReused == false)
    }

    @Test func aClockThatWentBackwardsExpiresTheSession() {
        let stored = StoredSession(cookie: Self.cookieA, issuedAt: Date(timeIntervalSince1970: 2_000))
        #expect(!stored.isUsable(at: Date(timeIntervalSince1970: 1_000)))
        #expect(stored.isUsable(at: Date(timeIntervalSince1970: 2_000)))
        #expect(stored.isUsable(at: Date(timeIntervalSince1970: 2_000 + 25 * 60 - 1)))
        #expect(!stored.isUsable(at: Date(timeIntervalSince1970: 2_000 + 25 * 60)))
    }

    @Test func aRestartedGatewayIssuesANewSessionBeforeThePOST() async {
        sessions.save(StoredSession(cookie: Self.cookieA, issuedAt: clock.now))
        sessionReply(cookie: Self.cookieB)                  // Gateway forgot A
        transport.enqueue(path: "/api/v1/mute", .json(200, Self.ok))
        let record = await runner().run(.mute)
        #expect(transport.posts.first?.value(forHTTPHeaderField: "Cookie") == "xs8_session=\(Self.cookieB)")
        #expect(sessions.load()?.cookie == Self.cookieB)
        #expect(record.sessionReused == false)
        #expect(record.ok)
    }

    @Test func aRejectedSessionOnPOSTIsForgottenAndNotRetried() async {
        sessionReply(cookie: Self.cookieA)
        transport.enqueue(path: "/api/v1/mute", .json(401, #"{"error":"Authentication and session required"}"#))
        let record = await runner().run(.mute)
        #expect(transport.posts.count == 1)
        #expect(sessions.load() == nil)
        #expect(record.summary == "MUTE SESSION EXPIRED · NOTHING SENT")
    }

    @Test func everyTapReadsAFreshTokenEvenAfterASuccessor() async {
        let runner = runner()
        sessionReply(cookie: Self.cookieA, token: "token-1")
        transport.enqueue(path: "/api/v1/volume/step", .json(200, #"{"result":"SERVICE_TX_OK","csrf_token":"successor"}"#))
        _ = await runner.run(.volumeUp)
        clock.advance(seconds: 1)
        sessionReply(token: "token-2")
        transport.enqueue(path: "/api/v1/volume/step", .json(200, Self.ok))
        _ = await runner.run(.volumeUp)
        #expect(transport.paths == ["/api/v1/session", "/api/v1/volume/step", "/api/v1/session", "/api/v1/volume/step"])
        #expect(transport.posts.map { $0.value(forHTTPHeaderField: "X-CSRF-Token") } == ["token-1", "token-2"])
    }

    // MARK: Availability, spacing and taps

    @Test func unavailableControlsSendNothing() async {
        sessionReply(cookie: Self.cookieA, mute: false)
        let record = await runner().run(.mute)
        #expect(transport.posts.isEmpty)
        #expect(record.summary == "MUTE NOT AVAILABLE · NOTHING SENT")
    }

    @Test func allGatesOffSendsNothing() async {
        transport.enqueue(.session, .json(404, #"{"error":"Not found"}"#))
        let record = await runner().run(.volumeDown)
        #expect(transport.posts.isEmpty)
        #expect(record.summary == "VOL− CONTROLS DISABLED · NOTHING SENT")
    }

    @Test func volumeTapsInside250msAreDropped() async {
        let runner = runner()
        transport.setDefault(path: ReadEndpoint.session.rawValue,
                             .json(200, #"{"csrf_token":"t","mute_available":true,"volume_step_available":true}"#))
        transport.setDefault(path: "/api/v1/volume/step", .json(200, Self.ok))
        _ = await runner.run(.volumeUp)
        clock.advance(seconds: 0.1)
        let early = await runner.run(.volumeUp)
        clock.advance(seconds: 0.2)
        _ = await runner.run(.volumeUp)
        #expect(early.summary == "VOL+ TOO FAST · NOTHING SENT")
        #expect(transport.posts(to: "/api/v1/volume/step").count == 2)
    }

    @Test func overlappingTapsAreDroppedNotQueued() async {
        let runner = runner()
        transport.setDefault(path: ReadEndpoint.session.rawValue,
                             .json(200, #"{"csrf_token":"t","mute_available":true,"volume_step_available":true}"#))
        transport.enqueue(path: "/api/v1/mute", .json(200, Self.ok, delayMs: 150))
        async let first = runner.run(.mute)
        try? await Task.sleep(for: .milliseconds(30))
        async let second = runner.run(.mute)
        async let third = runner.run(.volumeUp)
        let results = await [first, second, third]
        #expect(transport.posts.count == 1)
        #expect(results[0].ok)
        #expect(results[1].summary == "MUTE ANOTHER TAP IN PROGRESS · NOTHING SENT")
        #expect(results[2].summary == "VOL+ ANOTHER TAP IN PROGRESS · NOTHING SENT")
    }

    // MARK: Outcomes

    @Test(arguments: [
        (StubTransport.Reply.json(429, #"{"result":"TOO_FAST","min_interval_ms":250}"#), "VOL+ TOO FAST · NOTHING SENT"),
        (.json(409, #"{"result":"VOLUME_BUSY"}"#), "VOL+ VOLUME BUSY · NOTHING SENT"),
        (.json(409, #"{"result":"NOT_ELIGIBLE"}"#), "VOL+ NOT ELIGIBLE · NOTHING SENT"),
        (.json(502, #"{"result":"SERVICE_UNCERTAIN","physical_check_required":true}"#),
         "VOL+ SERVICE UNCERTAIN · UNCERTAIN · CHECK RECEIVER"),
        (.html(504), "VOL+ HTTP 504 · UNCERTAIN · CHECK RECEIVER"),
        (.json(403, #"{"error":"Invalid CSRF token"}"#), "VOL+ INVALID CSRF TOKEN · UNCERTAIN · CHECK RECEIVER"),
        (.json(403, #"{"error":"HTTPS Origin required"}"#), "VOL+ ORIGIN REJECTED · NOTHING SENT"),
        (.json(409, #"{"result":"FAULT_LATCHED","physical_check_required":true}"#),
         "VOL+ FAULT LATCHED · CHECK RECEIVER · RESTART REQUIRED"),
        (.failure(.timedOut), "VOL+ NO REPLY (TIMEOUT) · UNCERTAIN · CHECK RECEIVER"),
        (.failure(.connectionLost), "VOL+ CONNECTION LOST · UNCERTAIN · CHECK RECEIVER"),
    ])
    func outcomesFollowThePolicyWithoutRetries(reply: StubTransport.Reply, summary: String) async {
        sessionReply(cookie: Self.cookieA)
        transport.enqueue(path: "/api/v1/volume/step", reply)
        let record = await runner().run(.volumeUp)
        #expect(record.summary == summary)
        #expect(!record.ok)
        #expect(transport.posts.count == 1)                // never retried
    }

    @Test func aFailedSessionReadSendsNothing() async {
        transport.enqueue(.session, .failure(.unreachable("no route")))
        let record = await runner().run(.mute)
        #expect(transport.posts.isEmpty)
        #expect(record.summary == "MUTE PI NOT REACHED · NOTHING SENT")
    }

    @Test func aRejectedPasswordSendsNothing() async {
        transport.enqueue(.session, .http(401, Data(), ["WWW-Authenticate": "Basic realm=\"Kenwood XS Remote\""]))
        let record = await runner().run(.mute)
        #expect(transport.posts.isEmpty)
        #expect(record.summary == "MUTE PASSWORD REJECTED · NOTHING SENT")
    }

    @Test func aLockedDeviceSendsNothing() async {
        let runner = runner(credentials: { throw KeychainError(status: errSecInteractionNotAllowed) })
        let record = await runner.run(.mute)
        #expect(transport.requests.isEmpty)
        #expect(record.summary == "MUTE DEVICE LOCKED · NOTHING SENT")
    }

    @Test func missingCredentialsSendNothing() async {
        let record = await runner(credentials: { nil }).run(.volumeDown)
        #expect(transport.requests.isEmpty)
        #expect(record.summary == "VOL− NO CREDENTIALS · OPEN THE APP · NOTHING SENT")
    }
}

/// The real keychain store, in a group the test host can use.
@Suite(.serialized) struct KeychainSessionCookieStoreTests {
    @Test func roundTripAndDelete() {
        let store = KeychainSessionCookieStore(group: GatewayConfiguration.sharedKeychainGroup)
        let session = StoredSession(cookie: "cookie-0123456789abcdef", issuedAt: Date(timeIntervalSince1970: 1_800_000_000))
        store.save(session)
        #expect(store.load() == session)
        store.delete()
        #expect(store.load() == nil)
    }
}
