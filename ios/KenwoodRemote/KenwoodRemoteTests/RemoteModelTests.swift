import Foundation
import Testing
@testable import KenwoodRemote

final class MemoryCredentialStore: CredentialStore, @unchecked Sendable {
    private let lock = NSLock()
    private var stored: BasicCredentials?
    init(_ credentials: BasicCredentials? = nil) { stored = credentials }
    func load() throws -> BasicCredentials? { lock.withLock { stored } }
    func save(_ credentials: BasicCredentials) throws { lock.withLock { stored = credentials } }
    func delete() throws { lock.withLock { stored = nil } }
}

@MainActor @Suite struct RemoteModelTests {
    static let credentials = BasicCredentials(username: "remote", password: "secret")
    let transport = StubTransport()

    func model(store: MemoryCredentialStore = MemoryCredentialStore(credentials)) -> RemoteModel {
        RemoteModel(credentialStore: store) { [transport] _ in
            GatewayClient(baseURL: URL(string: "https://woodland-pi.local")!, transport: transport)
        }
    }

    @Test func withoutCredentialsNothingIsRequested() async {
        let model = model(store: MemoryCredentialStore())
        #expect(model.connection == .needsCredentials)
        await model.refresh()
        #expect(transport.requests.isEmpty)
        #expect(model.connection == .needsCredentials)
    }

    @Test func healthyRefreshGoesOnlineAndExpandsTheCDPanel() async {
        let model = model()
        #expect(model.connection == .connecting)
        transport.enqueueHealthyRefresh()
        await model.refresh()

        #expect(model.connection == .online)
        #expect(model.sourceIndicator.highlight(for: .cd) == .fresh)
        #expect(model.powerIndicator == .on)
        // The fixture's MONITOR observation is LAST_KNOWN: neutral, not OFF.
        #expect(model.monitorIndicator == .unknown)
        #expect(model.panel.visiblePanel == .cd && model.panel.isExpanded)
        #expect(model.capabilities?.operation(.cdTrackSelect)?.enabled == true)
    }

    @Test func serviceUnavailableIsShownAndNothingIsHighlighted() async {
        let model = model()
        transport.enqueueJSON(.status, status: 503, fixture: "status-unavailable")
        await model.refresh()

        #expect(model.connection == .serviceUnavailable("XS8 service unavailable"))
        #expect(model.sourceIndicator.quality == .connectionLost)
        #expect(model.capabilities == nil)
    }

    @Test func aFailedRefreshClearsEveryIndicator() async {
        let model = model()
        transport.enqueueHealthyRefresh()
        await model.refresh()
        transport.enqueue(.status, .failure(.timedOut))
        await model.refresh()

        #expect(model.connection == .failed("Request timed out"))
        #expect(Source.allCases.allSatisfy { model.sourceIndicator.highlight(for: $0) == .none })
        #expect(model.powerIndicator == .unknown)
        #expect(model.capabilities == nil)
        // The panel stays where it was; it never claims state.
        #expect(model.panel.visiblePanel == .cd)
    }

    @Test(arguments: [
        (GatewayIssue.authenticationRejected, ConnectionState.authenticationRejected),
        (.transport(.tlsRejected("bad CA")), .tlsRejected("bad CA")),
        (.transport(.unreachable("no host")), .unreachable("no host")),
        (.gatewayUnavailable(502), .failed("Gateway unavailable (HTTP 502)")),
    ])
    func issuesMapToConnectionStates(issue: GatewayIssue, expected: ConnectionState) {
        #expect(ConnectionState(issue) == expected)
    }

    @Test func concurrentRefreshesShareOneRead() async {
        let model = model()
        transport.enqueueHealthyRefresh()
        async let first: Void = model.refresh()
        async let second: Void = model.refresh()
        _ = await (first, second)
        #expect(transport.requests.count == 3)
    }

    @Test func savingCredentialsStoresThemAndConnects() async throws {
        let store = MemoryCredentialStore()
        let model = model(store: store)
        try model.saveCredentials(Self.credentials)

        #expect(try store.load() == Self.credentials)
        #expect(model.savedUsername == "remote")
        #expect(model.connection == .connecting)
    }

    @Test func forgettingCredentialsStopsReading() async throws {
        let store = MemoryCredentialStore(Self.credentials)
        let model = model(store: store)
        try model.forgetCredentials()
        await model.refresh()

        #expect(try store.load() == nil)
        #expect(model.connection == .needsCredentials)
        #expect(transport.requests.isEmpty)
    }

    @Test func aMissingCABecomesAConfigurationError() {
        let model = RemoteModel(credentialStore: MemoryCredentialStore(Self.credentials)) { _ in
            throw CertificateAuthorityError.missing("KenwoodCA.der")
        }
        guard case .configurationError(let message) = model.connection else {
            Issue.record("expected configurationError, got \(model.connection)")
            return
        }
        #expect(message.contains("KenwoodCA.der"))
    }
}

/// Real Keychain on the simulator, under a test-only server name.
@Suite(.serialized) struct KeychainCredentialStoreTests {
    let store = KeychainCredentialStore(server: "keychain-test.kenwood.invalid")

    @Test func roundTripReplaceAndDelete() throws {
        try store.delete()
        #expect(try store.load() == nil)
        try store.save(BasicCredentials(username: "remote", password: "first"))
        try store.save(BasicCredentials(username: "remote2", password: "second ü"))
        #expect(try store.load() == BasicCredentials(username: "remote2", password: "second ü"))
        try store.delete()
        #expect(try store.load() == nil)
    }
}
