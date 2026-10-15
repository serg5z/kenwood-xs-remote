import Foundation
import Testing
@testable import KenwoodRemote

/// The authenticated write path: headers, CSRF rotation and recovery, the
/// serial lane, and no retries.
@Suite struct GatewayClientControlTests {
    let transport = StubTransport()
    let client: GatewayClient

    init() {
        client = GatewayClient(baseURL: URL(string: "https://woodland-pi.local")!, transport: transport)
    }

    static let ok = #"{"result":"SERVICE_TX_OK","receiver_execution":"UNVERIFIED"}"#

    @Test func postCarriesExactlyTheGatewayHeaders() async throws {
        transport.enqueueHealthyRefresh(cookie: "cookie-value-0123456789")
        _ = await client.refresh()
        transport.enqueue(path: "/api/v1/mute", .json(200, Self.ok))
        _ = await client.send(.mute)

        let post = try #require(transport.posts.first)
        #expect(post.httpMethod == "POST")
        #expect(post.url?.absoluteString == "https://woodland-pi.local/api/v1/mute")
        #expect(post.value(forHTTPHeaderField: "Origin") == "https://woodland-pi.local")
        #expect(post.value(forHTTPHeaderField: "Content-Type") == "application/json")
        #expect(post.value(forHTTPHeaderField: "X-CSRF-Token") == "token-from-session-0001")
        #expect(post.value(forHTTPHeaderField: "Cookie") == "xs8_session=cookie-value-0123456789")
        #expect(post.value(forHTTPHeaderField: "Authorization") == nil)
        #expect(post.httpBody == Data("{}".utf8))
        #expect(post.timeoutInterval == 6)
    }

    @Test func withoutATokenTheSessionIsReadFirst() async {
        transport.enqueueJSON(.session, fixture: "session")
        transport.enqueue(path: "/api/v1/mute", .json(200, Self.ok))
        _ = await client.send(.mute)
        #expect(transport.paths == ["/api/v1/session", "/api/v1/mute"])
        #expect(transport.posts.first?.value(forHTTPHeaderField: "X-CSRF-Token") == "token-from-session-0001")
    }

    @Test func theReturnedSuccessorIsUsedOnceForTheNextControl() async {
        transport.enqueueJSON(.session, fixture: "session")
        transport.enqueue(path: "/api/v1/cd/track",
                          .json(200, #"{"result":"SERVICE_TX_OK","csrf_token":"successor-1"}"#))
        transport.enqueue(path: "/api/v1/cd/track", .json(200, Self.ok))
        transport.enqueue(.session, .json(200, #"{"csrf_token":"fresh-2","mute_available":true}"#))
        transport.enqueue(path: "/api/v1/mute", .json(200, Self.ok))

        _ = await client.send(.cdTrack(next: true))
        _ = await client.send(.cdTrack(next: true))   // uses successor-1, gets none back
        _ = await client.send(.mute)                   // must fetch a fresh token

        #expect(transport.paths == ["/api/v1/session", "/api/v1/cd/track", "/api/v1/cd/track",
                                    "/api/v1/session", "/api/v1/mute"])
        #expect(transport.posts.map { $0.value(forHTTPHeaderField: "X-CSRF-Token") } ==
                ["token-from-session-0001", "successor-1", "fresh-2"])
    }

    @Test func aTokenIsNeverReusedAfterARefusal() async {
        transport.setDefault(path: ReadEndpoint.session.rawValue,
                             .json(200, #"{"csrf_token":"same-token"}"#))
        transport.enqueue(path: "/api/v1/source", .json(429, #"{"result":"TOO_FAST"}"#))
        transport.enqueue(path: "/api/v1/source", .json(200, Self.ok))
        _ = await client.send(.source(.cd))
        _ = await client.send(.source(.cd))
        // Even when the session hands back the same value, it was re-read.
        #expect(transport.paths == ["/api/v1/session", "/api/v1/source", "/api/v1/session", "/api/v1/source"])
    }

    @Test func invalidCSRFIsReportedOnceAndNeverRetried() async {
        transport.enqueueJSON(.session, fixture: "session")
        transport.enqueue(path: "/api/v1/volume/step", .json(403, #"{"error":"Invalid CSRF token"}"#))
        let exchange = await client.send(.volumeStep(up: true))
        #expect(ControlOutcome(exchange) == .uncertain("INVALID CSRF TOKEN"))
        #expect(transport.posts.count == 1)
    }

    @Test func transportFailureIsNeverRetried() async {
        transport.enqueueJSON(.session, fixture: "session")
        transport.enqueue(path: "/api/v1/power", .failure(.timedOut))
        let exchange = await client.send(.powerToggle(expected: nil))
        #expect(exchange == .transportFailed(.timedOut))
        #expect(transport.posts.count == 1)
    }

    @Test func gatewaySessionLossClearsTheCookie() async {
        transport.enqueueHealthyRefresh(cookie: "cookie-value-0123456789")
        _ = await client.refresh()
        transport.enqueue(path: "/api/v1/mute", .json(401, #"{"error":"Authentication and session required"}"#))
        _ = await client.send(.mute)
        #expect(await client.sessionCookie == nil)
    }

    @Test func aCancelledGuardSendsNothingAndKeepsTheToken() async {
        transport.enqueueJSON(.session, fixture: "session")
        let guardian = SendGuard()
        guardian.cancel()
        let exchange = await client.send(.cdSeek(.start(direction: "FORWARD")), guard: guardian)
        #expect(exchange == .notAttempted("CANCELLED BEFORE SEND"))
        #expect(transport.posts.isEmpty)

        transport.enqueue(path: "/api/v1/mute", .json(200, Self.ok))
        _ = await client.send(.mute)
        // The unused token was kept; no second session read.
        #expect(transport.paths == ["/api/v1/session", "/api/v1/mute"])
    }

    @Test func invalidRequestsSendNothing() async {
        let exchange = await client.send(.cdTrackSelect(40))
        #expect(exchange == .notAttempted("INVALID REQUEST"))
        #expect(transport.requests.isEmpty)
    }

    @Test func noSessionTokenMeansNothingSent() async {
        transport.enqueue(.session, .json(404, #"{"error":"Not found"}"#))
        let exchange = await client.send(.mute)
        #expect(exchange == .notAttempted("NO SESSION TOKEN"))
        #expect(transport.posts.isEmpty)
    }

    @Test func readsAndControlsNeverInterleave() async {
        transport.enqueue(.status, .http(200, Fixture.data("status-connected"),
                                         ["Content-Type": "application/json"], delayMs: 150))
        transport.enqueueJSON(.session, fixture: "session")
        transport.enqueueJSON(.capabilities, fixture: "capabilities")
        transport.enqueue(path: "/api/v1/mute", .json(200, Self.ok))

        async let read = client.refresh()
        try? await Task.sleep(for: .milliseconds(20))
        async let control = client.send(.mute)
        _ = await (read, control)

        #expect(transport.paths == ["/api/v1/status", "/api/v1/session", "/api/v1/capabilities", "/api/v1/mute"])
        #expect(transport.posts.first?.value(forHTTPHeaderField: "X-CSRF-Token") == "token-from-session-0001")
    }

    @Test func nonJSONRepliesAreKeptUnparsed() async {
        transport.enqueueJSON(.session, fixture: "session")
        transport.enqueue(path: "/api/v1/mute", .html(502))
        let exchange = await client.send(.mute)
        #expect(exchange == .response(status: 502, reply: nil, gatewayJSON: false))
        #expect(ControlOutcome(exchange) == .uncertain("HTTP 502"))
    }
}
