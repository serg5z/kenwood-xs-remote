import Foundation
import Security

protocol CredentialStore: Sendable {
    func load() throws -> BasicCredentials?
    func save(_ credentials: BasicCredentials) throws
    func delete() throws
}

struct KeychainError: Error, Equatable {
    let status: OSStatus
}

/// nginx Basic Auth credentials as one internet-password item for the Gateway
/// host: available only while the device is unlocked, never synced or migrated
/// to another device.
struct KeychainCredentialStore: CredentialStore {
    let server: String

    private var baseQuery: [String: Any] {
        [kSecClass as String: kSecClassInternetPassword,
         kSecAttrServer as String: server,
         kSecAttrProtocol as String: kSecAttrProtocolHTTPS]
    }

    func load() throws -> BasicCredentials? {
        var query = baseQuery
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

    func save(_ credentials: BasicCredentials) throws {
        try delete()
        var item = baseQuery
        item[kSecAttrAccount as String] = credentials.username
        item[kSecValueData as String] = Data(credentials.password.utf8)
        item[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
        item[kSecAttrLabel as String] = "Kenwood Remote (\(server))"
        let status = SecItemAdd(item as CFDictionary, nil)
        guard status == errSecSuccess else { throw KeychainError(status: status) }
    }

    func delete() throws {
        let status = SecItemDelete(baseQuery as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else {
            throw KeychainError(status: status)
        }
    }
}
