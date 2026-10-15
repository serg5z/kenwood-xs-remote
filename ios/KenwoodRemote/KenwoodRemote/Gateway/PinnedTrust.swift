import CryptoKit
import Foundation
import Security

enum CertificateAuthorityError: Error, Equatable {
    case missing(String)
    case invalid
    case fingerprintMismatch(String)
}

/// Loads the private CA bundled with the app (a local, untracked build input) and
/// checks it against the configured fingerprint before it is ever used.
enum PinnedCertificateAuthority {
    static func load(_ configuration: GatewayConfiguration,
                     bundle: Bundle = .main) throws -> SecCertificate {
        guard let url = bundle.url(forResource: configuration.caResourceName,
                                   withExtension: "der") else {
            throw CertificateAuthorityError.missing("\(configuration.caResourceName).der")
        }
        return try certificate(der: Data(contentsOf: url),
                               expectedSHA256: configuration.caSHA256)
    }

    static func certificate(der: Data, expectedSHA256: String) throws -> SecCertificate {
        let digest = SHA256.hash(data: der).map { String(format: "%02x", $0) }.joined()
        guard digest == expectedSHA256.lowercased() else {
            throw CertificateAuthorityError.fingerprintMismatch(digest)
        }
        guard let certificate = SecCertificateCreateWithData(nil, der as CFData) else {
            throw CertificateAuthorityError.invalid
        }
        return certificate
    }
}

/// Server trust for the one Gateway host: the bundled private CA is the only
/// anchor (system roots are not consulted), and the SSL policy checks the name.
struct PinnedTrustEvaluator: @unchecked Sendable {
    // SecCertificate is immutable; it is only read after initialization.
    let host: String
    let anchor: SecCertificate
    /// Tests only: evaluate at a fixed date so fixtures never expire.
    var verifyDate: Date?

    /// nil when the server is trusted; otherwise why it is not.
    func evaluate(_ trust: SecTrust, host challengeHost: String) -> TrustFailure? {
        guard challengeHost == host else { return .unexpectedHost(challengeHost) }
        guard SecTrustSetPolicies(trust, SecPolicyCreateSSL(true, host as CFString)) == errSecSuccess,
              SecTrustSetAnchorCertificates(trust, [anchor] as CFArray) == errSecSuccess,
              SecTrustSetAnchorCertificatesOnly(trust, true) == errSecSuccess,
              SecTrustSetNetworkFetchAllowed(trust, false) == errSecSuccess else {
            return .rejected("trust configuration failed")
        }
        if let verifyDate, SecTrustSetVerifyDate(trust, verifyDate as CFDate) != errSecSuccess {
            return .rejected("trust date configuration failed")
        }
        var error: CFError?
        guard SecTrustEvaluateWithError(trust, &error) else {
            return .rejected(error.map { CFErrorCopyDescription($0) as String }
                             ?? "certificate not trusted")
        }
        return nil
    }
}

enum TrustFailure: Error, Equatable, Sendable {
    case unexpectedHost(String)
    case rejected(String)

    var message: String {
        switch self {
        case .unexpectedHost(let host): "Unexpected host \(host)"
        case .rejected(let reason): reason
        }
    }
}
