import Network
import SwiftUI
import UIKit

@main
struct KenwoodRemoteApp: App {
    @State private var model: RemoteModel = Self.isHostingUnitTests ? .inert() : .live(
        beginBackgroundTask: { name in
            // Lets a hold's RELEASE finish after the app leaves the foreground.
            let task = BackgroundTask()
            task.identifier = UIApplication.shared.beginBackgroundTask(withName: name) { task.end() }
            return { task.end() }
        })
    private let networkPath = Self.isHostingUnitTests ? nil : NetworkPathWatcher()

    var body: some Scene {
        WindowGroup {
            MainRemoteView(model: model, networkPath: networkPath)
        }
    }

    /// Unit tests run inside this app; it must not read the real Keychain or
    /// contact the Pi while they do.
    private static var isHostingUnitTests: Bool {
        ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"] != nil
    }
}

/// One UIKit background task, ended exactly once (finished or expired).
@MainActor
private final class BackgroundTask {
    var identifier = UIBackgroundTaskIdentifier.invalid

    func end() {
        guard identifier != .invalid else { return }
        UIApplication.shared.endBackgroundTask(identifier)
        identifier = .invalid
    }
}

/// Reports whether any network path is usable. Losing it ends a running hold
/// at once instead of waiting for heartbeats to fail.
final class NetworkPathWatcher: Sendable {
    func updates() -> AsyncStream<Bool> {
        AsyncStream { continuation in
            let monitor = NWPathMonitor()
            monitor.pathUpdateHandler = { path in continuation.yield(path.status == .satisfied) }
            continuation.onTermination = { _ in monitor.cancel() }
            monitor.start(queue: DispatchQueue(label: "KenwoodRemote.NetworkPath"))
        }
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
