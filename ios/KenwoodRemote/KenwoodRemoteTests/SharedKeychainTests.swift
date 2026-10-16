import Foundation
import Security
import Testing
@testable import KenwoodRemote

/// Keychain sharing for the Control Center extension, against the simulator's
/// real keychain with the app's entitlements, under a test-only server name.
@Suite(.serialized) struct SharedKeychainTests {
    static let server = "migration-test.kenwood.invalid"
    let shared = KeychainCredentialStore(server: server, accessGroup: GatewayConfiguration.sharedKeychainGroup)
    let legacyOnly = KeychainCredentialStore(server: server, accessGroup: GatewayConfiguration.legacyKeychainGroup)
    var migrating: KeychainCredentialStore {
        KeychainCredentialStore(server: Self.server, accessGroup: GatewayConfiguration.sharedKeychainGroup,
                                legacyAccessGroup: GatewayConfiguration.legacyKeychainGroup)
    }
    let credentials = BasicCredentials(username: "remote", password: "pässword")

    func reset() throws {
        try shared.delete()
        try legacyOnly.delete()
    }

    @Test func legacyLoginMovesToTheSharedGroupOnce() throws {
        try reset()
        try legacyOnly.save(credentials)                  // as the previous build stored it
        #expect(!shared.exists())

        #expect(try migrating.load() == credentials)       // first launch of the new build
        #expect(shared.exists())
        #expect(try shared.load() == credentials)          // what the extension reads
        #expect(try legacyOnly.load() == nil)              // the old copy is gone
        #expect(try migrating.load() == credentials)       // later launches: unchanged
        try reset()
    }

    @Test func sharedItemWinsOverALeftoverLegacyItem() throws {
        try reset()
        try shared.save(credentials)
        try legacyOnly.save(BasicCredentials(username: "old", password: "old"))
        #expect(try migrating.load() == credentials)
        try reset()
    }

    @Test func savingAndForgettingUseTheSharedGroup() throws {
        try reset()
        try migrating.save(credentials)
        #expect(try shared.load() == credentials)
        try migrating.delete()
        #expect(!shared.exists())
        #expect(try migrating.load() == nil)
    }

    @Test func diagnosticRecordRoundTrip() {
        let record = ControlDiagnostic(date: Date(timeIntervalSince1970: 1_800_000_000), processID: 42,
                                       processAgeMs: 180, invocationInProcess: 1, keychainMs: 3,
                                       requestMs: 240, totalMs: 260, ok: true,
                                       summary: "GATEWAY OK · XS8 IDLE · SOURCE CD FRESH · 240 ms")
        let group = GatewayConfiguration.sharedKeychainGroup
        #expect(record.save(group: group) == errSecSuccess)
        #expect(ControlDiagnostic.load(group: group) == record)
    }
}

@Suite struct StatusOnlyReadTests {
    @Test func statusReadIsOneGETWithoutASession() async {
        let transport = StubTransport()
        transport.enqueueJSON(.status, fixture: "status-connected")
        let client = GatewayClient(baseURL: URL(string: "https://woodland-pi.local")!, transport: transport)
        let result = await client.status()
        #expect((try? result.get())?.serviceConnected == true)
        #expect(transport.paths == ["/api/v1/status"])
        #expect(transport.requests.allSatisfy { $0.httpMethod == "GET" && $0.value(forHTTPHeaderField: "Cookie") == nil })
    }

    @Test func statusReadReportsNginxRefusal() async {
        let transport = StubTransport()
        transport.enqueue(.status, .http(401, Data(), ["WWW-Authenticate": "Basic realm=\"Kenwood XS Remote\""]))
        let client = GatewayClient(baseURL: URL(string: "https://woodland-pi.local")!, transport: transport)
        #expect(await client.status() == .failure(.authenticationRejected))
    }
}
