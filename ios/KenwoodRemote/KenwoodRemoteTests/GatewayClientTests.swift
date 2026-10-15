import Foundation
import Testing
@testable import KenwoodRemote

@Suite struct GatewayClientTests {
    let transport = StubTransport()
    var client: GatewayClient {
        GatewayClient(baseURL: URL(string: "https://woodland-pi.local")!, transport: transport)
    }

    @Test func healthyRefreshReadsStatusSessionCapabilitiesInOrder() async throws {
        transport.enqueueHealthyRefresh()
        let snapshot = try await client.refresh().get()

        #expect(transport.paths == ["/api/v1/status", "/api/v1/session", "/api/v1/capabilities"])
        #expect(snapshot.status.serviceConnected)
        #expect(snapshot.session?.cdTrackSelect == true)
        #expect(snapshot.capabilities?.operation(.cdTrackSelect)?.maxTrack == 39)
        #expect(snapshot.controlsConfigured)
    }

    @Test func everyRequestIsABodylessGETWithoutControlHeaders() async throws {
        let client = client
        transport.enqueueHealthyRefresh()
        _ = await client.refresh()
        transport.enqueueHealthyRefresh()
        _ = await client.refresh()

        #expect(transport.requests.count == 6)
        for request in transport.requests {
            #expect(request.httpMethod == "GET")
            #expect(request.httpBody == nil && request.httpBodyStream == nil)
            #expect(request.value(forHTTPHeaderField: "X-CSRF-Token") == nil)
            #expect(request.value(forHTTPHeaderField: "Origin") == nil)
            #expect(request.value(forHTTPHeaderField: "Authorization") == nil)
            #expect(request.url?.host == "woodland-pi.local")
            #expect(request.url?.scheme == "https")
            #expect(request.timeoutInterval == GatewayClient.readTimeout)
        }
    }

    @Test func sessionCookieIsCapturedAndSentAsTheOnlyCookie() async throws {
        let client = client
        transport.enqueueHealthyRefresh(cookie: "first-session-value-0123")
        _ = await client.refresh()
        transport.enqueueHealthyRefresh(cookie: nil)
        _ = await client.refresh()

        let cookies = transport.requests.map { $0.value(forHTTPHeaderField: "Cookie") }
        // First status and session go out without a cookie; the capabilities
        // request after the session already carries it, and so does every later one.
        #expect(cookies == [nil, nil,
                            "xs8_session=first-session-value-0123",
                            "xs8_session=first-session-value-0123",
                            "xs8_session=first-session-value-0123",
                            "xs8_session=first-session-value-0123"])
    }

    @Test func aNewSessionCookieReplacesTheOldOne() async throws {
        let client = client
        transport.enqueueHealthyRefresh(cookie: "old-session-value-012345")
        _ = await client.refresh()
        // A Gateway restart forgets sessions; /session then issues a new cookie.
        transport.enqueueHealthyRefresh(cookie: "new-session-value-012345")
        _ = await client.refresh()
        #expect(await client.sessionCookie == "new-session-value-012345")
    }

    @Test(arguments: [
        ("xs8_session=abcDEF012_-abcDEF012_-; Path=/; Secure", "abcDEF012_-abcDEF012_-"),
        ("other=1; Path=/, xs8_session=abcDEF012_-abcDEF012_-; Secure", "abcDEF012_-abcDEF012_-"),
        ("xs8_session=short; Path=/", nil),
        ("xs8_session=bad value with spaces; Path=/", nil),
        ("not_xs8_session=abcDEF012_-abcDEF012_-; Path=/", nil),
    ])
    func sessionCookieParsing(header: String, expected: String?) {
        let response = HTTPURLResponse(url: URL(string: "https://woodland-pi.local/")!,
                                       statusCode: 200, httpVersion: nil,
                                       headerFields: ["Set-Cookie": header])!
        #expect(GatewayClient.sessionCookie(from: response) == expected)
    }

    @Test func serviceUnavailableIsASnapshotNotAFailure() async throws {
        transport.enqueueJSON(.status, status: 503, fixture: "status-unavailable")
        let snapshot = try await client.refresh().get()

        #expect(!snapshot.status.serviceConnected)
        #expect(snapshot.status.error == "XS8 service unavailable")
        #expect(snapshot.session == nil)
        #expect(transport.paths == ["/api/v1/status"])
    }

    @Test func nginxBasicAuthRefusalIsAuthenticationRejected() async {
        transport.enqueue(.status, .http(401, Data("<html>401</html>".utf8),
                                         ["Content-Type": "text/html",
                                          "WWW-Authenticate": "Basic realm=\"Kenwood XS Remote\""]))
        #expect(await client.refresh() == .failure(.authenticationRejected))
    }

    @Test func nginxUpstreamFailureIsGatewayUnavailable() async {
        transport.enqueue(.status, .http(502, Data("<html>502</html>".utf8), ["Content-Type": "text/html"]))
        #expect(await client.refresh() == .failure(.gatewayUnavailable(502)))
    }

    @Test func hostMismatchIsReported() async {
        transport.enqueue(.status, .http(421, Data(), [:]))
        #expect(await client.refresh() == .failure(.hostMismatch))
    }

    @Test func allControlGatesOffMeansNoSessionAndNoCapabilities() async throws {
        transport.enqueueJSON(.status, fixture: "status-connected")
        transport.enqueue(.session, .http(404, Data(#"{"error":"Not found"}"#.utf8),
                                          ["Content-Type": "application/json; charset=utf-8"]))
        let snapshot = try await client.refresh().get()

        #expect(snapshot.session == nil)
        #expect(!snapshot.controlsConfigured)
        #expect(transport.paths == ["/api/v1/status", "/api/v1/session"])
    }

    @Test func capabilitiesFailureKeepsStatus() async throws {
        transport.enqueueJSON(.status, fixture: "status-connected")
        transport.enqueueJSON(.session, fixture: "session")
        transport.enqueue(.capabilities, .failure(.timedOut))
        let snapshot = try await client.refresh().get()

        #expect(snapshot.status.serviceConnected)
        #expect(snapshot.capabilities == nil)
        #expect(snapshot.controlInfoIssue == .transport(.timedOut))
    }

    @Test func transportFailureOnStatusFailsTheRefresh() async {
        transport.enqueue(.status, .failure(.unreachable("no route")))
        #expect(await client.refresh() == .failure(.transport(.unreachable("no route"))))
        #expect(transport.paths == ["/api/v1/status"])
    }

    @Test func malformedJSONIsInvalidBody() async {
        transport.enqueue(.status, .http(200, Data("{".utf8), ["Content-Type": "application/json"]))
        #expect(await client.refresh() == .failure(.invalidBody(.status)))
    }
}
