import SwiftUI

@main
struct KenwoodRemoteApp: App {
    @State private var model: RemoteModel = Self.isHostingUnitTests ? .inert() : .live()

    var body: some Scene {
        WindowGroup {
            MainRemoteView(model: model)
        }
    }

    /// Unit tests run inside this app; it must not read the real Keychain or
    /// contact the Pi while they do.
    private static var isHostingUnitTests: Bool {
        ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"] != nil
    }
}

private struct NoCredentials: CredentialStore {
    func load() throws -> BasicCredentials? { nil }
    func save(_ credentials: BasicCredentials) throws {}
    func delete() throws {}
}

private struct NoNetwork: Error {}

extension RemoteModel {
    /// A model that never connects: no stored credentials, no client.
    static func inert() -> RemoteModel {
        RemoteModel(credentialStore: NoCredentials()) { _ in throw NoNetwork() }
    }
}
