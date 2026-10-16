import Foundation
import Security

/// The Control Center extension's own Gateway session cookie and when the
/// Gateway issued it. Never a CSRF token: every tap reads `/session` for that.
struct StoredSession: Codable, Equatable, Sendable {
    let cookie: String
    let issuedAt: Date

    /// The Gateway fixes a session's expiry at creation (30 min, not extended by
    /// use). Stop using it locally after 25 min, so a cookie is never sent near
    /// the Gateway's limit.
    static let lifetime: TimeInterval = 25 * 60

    /// A clock that moved backwards makes the age negative: treated as expired.
    func isUsable(at now: Date) -> Bool {
        let age = now.timeIntervalSince(issuedAt)
        return age >= 0 && age < Self.lifetime
    }
}

protocol SessionCookieStore: Sendable {
    func load() -> StoredSession?
    func save(_ session: StoredSession)
    func delete()
}

/// One generic-password item in a keychain group only the extension holds
/// (its own App ID group), so the app's session and the extension's never
/// mix. Available only while unlocked, this device only.
struct KeychainSessionCookieStore: SessionCookieStore {
    let group: String
    private static let service = "local.woodland.KenwoodRemote.extension-session"

    private var query: [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: Self.service,
         kSecAttrAccount as String: "gateway",
         kSecAttrAccessGroup as String: group]
    }

    func load() -> StoredSession? {
        var query = query
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess,
              let data = item as? Data else { return nil }
        return try? JSONDecoder().decode(StoredSession.self, from: data)
    }

    func save(_ session: StoredSession) {
        guard let data = try? JSONEncoder().encode(session) else { return }
        SecItemDelete(query as CFDictionary)
        var item = query
        item[kSecValueData as String] = data
        item[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
        SecItemAdd(item as CFDictionary, nil)
    }

    func delete() {
        SecItemDelete(query as CFDictionary)
    }
}
