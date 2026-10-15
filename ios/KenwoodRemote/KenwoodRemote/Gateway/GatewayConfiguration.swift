import Foundation

/// The one known Gateway. Personal v1: no discovery, no alternate hosts.
struct GatewayConfiguration: Sendable {
    /// Must match the nginx `server_name` and the certificate's DNS SAN; nginx
    /// answers 421 for any other Host, including a bare IP address.
    let host: String
    /// SHA-256 of the private CA certificate's DER encoding. The bundled CA must
    /// match, so a wrong file can never become the trust anchor.
    let caSHA256: String
    let caResourceName: String

    var baseURL: URL { URL(string: "https://\(host)")! }

    static let woodland = GatewayConfiguration(
        host: "woodland-pi.local",
        // "Woodland Private CA", valid 2026-10-11 to 2036-10-08.
        caSHA256: "32c7ba0b091bc1e814446a5953426a5197c4ddf45cbe87da18c8103595779bbe",
        caResourceName: "KenwoodCA")
}
