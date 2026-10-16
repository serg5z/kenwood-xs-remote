import Foundation
import Security

/// The last run of the Control Center diagnostic, written by the extension and
/// shown in the app's Details. Kept as one small shared-keychain item because
/// there is no App Group. It holds no secret: timings and an outcome line.
struct ControlDiagnostic: Codable, Equatable, Sendable {
    var date: Date
    var processID: Int32
    /// Time since the extension process started: small means a cold start.
    var processAgeMs: Int?
    /// 1 for the first run in this process; higher means the process was reused.
    var invocationInProcess: Int
    var keychainMs: Int?
    /// The control POST (or the status GET) alone.
    var requestMs: Int?
    var totalMs: Int
    var ok: Bool
    var summary: String
    /// STATUS, MUTE, VOL+ or VOL− (absent in records from the read-only spike).
    var command: String?
    /// The `GET /session` before a control.
    var sessionMs: Int?
    /// Whether the stored extension session cookie was still accepted.
    var sessionReused: Bool?

    private static let service = "local.woodland.KenwoodRemote.control-diagnostic"

    private static func query(group: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: service,
         kSecAttrAccount as String: "last",
         kSecAttrAccessGroup as String: group]
    }

    /// Readable after the first unlock, so a run on a locked phone is recorded too.
    func save(group: String = GatewayConfiguration.sharedKeychainGroup) -> OSStatus {
        guard let data = try? JSONEncoder().encode(self) else { return errSecParam }
        SecItemDelete(Self.query(group: group) as CFDictionary)
        var item = Self.query(group: group)
        item[kSecValueData as String] = data
        item[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        return SecItemAdd(item as CFDictionary, nil)
    }

    static func load(group: String = GatewayConfiguration.sharedKeychainGroup) -> ControlDiagnostic? {
        var query = query(group: group)
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess,
              let data = item as? Data else { return nil }
        return try? JSONDecoder().decode(ControlDiagnostic.self, from: data)
    }
}
