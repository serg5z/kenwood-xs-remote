import Foundation
import Security

protocol CredentialStore: Sendable {
    func load() throws -> BasicCredentials?
    func save(_ credentials: BasicCredentials) throws
    func delete() throws
}

struct KeychainError: Error, Equatable {
    let status: OSStatus

    /// The device is locked and the item is available only while unlocked.
    var isLocked: Bool { status == errSecInteractionNotAllowed }
}

/// nginx Basic Auth credentials as one internet-password item for the Gateway
/// host: available only while the device is unlocked, never synced or migrated
/// to another device.
///
/// With `accessGroup`, the item lives in that keychain group, so the Control
/// Center extension can read it. `legacyAccessGroup` is where the app kept it
/// before keychain sharing (its own default group): `load()` moves an item
/// found only there into `accessGroup`, copying before deleting.
struct KeychainCredentialStore: CredentialStore {
    let server: String
    var accessGroup: String?
    var legacyAccessGroup: String?

    private func query(group: String?) -> [String: Any] {
        var query: [String: Any] = [kSecClass as String: kSecClassInternetPassword,
                                    kSecAttrServer as String: server,
                                    kSecAttrProtocol as String: kSecAttrProtocolHTTPS]
        if let group { query[kSecAttrAccessGroup as String] = group }
        return query
    }

    func load() throws -> BasicCredentials? {
        if let credentials = try read(group: accessGroup) { return credentials }
        guard let legacy = legacyAccessGroup, legacy != accessGroup,
              let credentials = try read(group: legacy) else { return nil }
        // One-time migration: the new copy must exist before the old one goes,
        // so a failure can never lose the login.
        try write(credentials, group: accessGroup)
        try? deleteItem(group: legacy)
        return credentials
    }

    func save(_ credentials: BasicCredentials) throws {
        try deleteItem(group: accessGroup)
        try write(credentials, group: accessGroup)
    }

    func delete() throws {
        try deleteItem(group: accessGroup)
        if let legacy = legacyAccessGroup, legacy != accessGroup { try deleteItem(group: legacy) }
    }

    /// Whether the item exists in `accessGroup`, without reading the password.
    func exists() -> Bool {
        var query = query(group: accessGroup)
        query[kSecReturnAttributes as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        return SecItemCopyMatching(query as CFDictionary, nil) == errSecSuccess
    }

    private func read(group: String?) throws -> BasicCredentials? {
        var query = query(group: group)
        query[kSecReturnAttributes as String] = true
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var item: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &item)
        if status == errSecItemNotFound { return nil }
        guard status == errSecSuccess else { throw KeychainError(status: status) }
        guard let attributes = item as? [String: Any],
              let username = attributes[kSecAttrAccount as String] as? String,
              let data = attributes[kSecValueData as String] as? Data,
              let password = String(data: data, encoding: .utf8) else {
            throw KeychainError(status: errSecDecode)
        }
        return BasicCredentials(username: username, password: password)
    }

    private func write(_ credentials: BasicCredentials, group: String?) throws {
        var item = query(group: group)
        item[kSecAttrAccount as String] = credentials.username
        item[kSecValueData as String] = Data(credentials.password.utf8)
        item[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
        item[kSecAttrLabel as String] = "Kenwood Remote (\(server))"
        let status = SecItemAdd(item as CFDictionary, nil)
        guard status == errSecSuccess else { throw KeychainError(status: status) }
    }

    private func deleteItem(group: String?) throws {
        let status = SecItemDelete(query(group: group) as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else {
            throw KeychainError(status: status)
        }
    }
}
