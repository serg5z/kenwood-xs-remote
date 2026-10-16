import AppIntents
import Foundation

/// Runs in the extension process. Every control requires an unlocked device
/// (Face ID or passcode from the Lock Screen). The button stays stateless:
/// nothing here reads or shows MUTE or volume state.
enum ControlCommands {
    static let runner = ExtensionCommandRunner(
        loadCredentials: {
            try KeychainCredentialStore(server: GatewayConfiguration.woodland.host,
                                        accessGroup: GatewayConfiguration.sharedKeychainGroup).load()
        },
        sessions: KeychainSessionCookieStore(group: GatewayConfiguration.extensionKeychainGroup),
        makeClient: { credentials in
            let configuration = GatewayConfiguration.woodland
            let anchor = try PinnedCertificateAuthority.load(configuration)
            let transport = URLSessionTransport(
                trust: PinnedTrustEvaluator(host: configuration.host, anchor: anchor),
                credentials: credentials)
            return GatewayClient(baseURL: configuration.baseURL, transport: transport)
        })

    /// One tap: run, record for the app's Details, report anything but a
    /// verified transmission as an error (whatever iOS chooses to show).
    static func perform(_ command: ExtensionCommand) async throws {
        var record = await runner.run(command)
        record.processAgeMs = StatusDiagnostic.processAgeMs()
        _ = record.save()
        guard record.ok else { throw DiagnosticFailure(message: record.summary) }
    }
}

struct MuteIntent: AppIntent {
    static let title: LocalizedStringResource = "Kenwood MUTE"
    static let description = IntentDescription("Sends one MUTE toggle. The receiver's mute state is not read back.")
    static let authenticationPolicy: IntentAuthenticationPolicy = .requiresAuthentication
    static let isDiscoverable = false

    func perform() async throws -> some IntentResult {
        try await ControlCommands.perform(.mute)
        return .result()
    }
}

enum VolumeDirection: String, AppEnum {
    case up, down

    static let typeDisplayRepresentation: TypeDisplayRepresentation = "Volume direction"
    static let caseDisplayRepresentations: [VolumeDirection: DisplayRepresentation] = [
        .up: "Volume +", .down: "Volume −",
    ]
}

struct VolumeStepIntent: AppIntent {
    static let title: LocalizedStringResource = "Kenwood Volume Step"
    static let description = IntentDescription("Sends one volume step. No press-and-hold.")
    static let authenticationPolicy: IntentAuthenticationPolicy = .requiresAuthentication
    static let isDiscoverable = false

    @Parameter(title: "Direction")
    var direction: VolumeDirection

    init() {}

    init(direction: VolumeDirection) {
        self.direction = direction
    }

    func perform() async throws -> some IntentResult {
        try await ControlCommands.perform(direction == .up ? .volumeUp : .volumeDown)
        return .result()
    }
}
