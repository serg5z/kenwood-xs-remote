import CryptoKit
import Foundation
import Security
import Testing
@testable import KenwoodRemote

/// Fixture PKI made only for these tests (no relation to the real CA):
/// test-ca signs test-leaf (woodland-pi.local) and test-other (other-host.local);
/// rogue-ca signs rogue-leaf (woodland-pi.local).
@Suite struct PinnedTrustTests {
    /// Inside every fixture's validity, so the tests never expire.
    static let verifyDate = ISO8601DateFormatter().date(from: "2027-01-01T00:00:00Z")!

    static func fingerprint(_ data: Data) -> String {
        SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    static func certificate(_ name: String) -> SecCertificate {
        SecCertificateCreateWithData(nil, Fixture.data(name) as CFData)!
    }

    static func trust(for leaf: String) -> SecTrust {
        var trust: SecTrust?
        // Policy is replaced by the evaluator; any SSL policy starts the object.
        SecTrustCreateWithCertificates(certificate(leaf), SecPolicyCreateSSL(true, nil), &trust)
        return trust!
    }

    var evaluator: PinnedTrustEvaluator {
        let caDER = Fixture.data("test-ca")
        let anchor = try! PinnedCertificateAuthority.certificate(der: caDER,
                                                                expectedSHA256: Self.fingerprint(caDER))
        return PinnedTrustEvaluator(host: "woodland-pi.local", anchor: anchor, verifyDate: Self.verifyDate)
    }

    @Test func acceptsTheGatewayLeafSignedByThePinnedCA() {
        #expect(evaluator.evaluate(Self.trust(for: "test-leaf"), host: "woodland-pi.local") == nil)
    }

    @Test func rejectsALeafFromAnotherCA() {
        let result = evaluator.evaluate(Self.trust(for: "rogue-leaf"), host: "woodland-pi.local")
        #expect(result != nil)
    }

    @Test func rejectsTheRightCAWithTheWrongName() {
        let result = evaluator.evaluate(Self.trust(for: "test-other"), host: "woodland-pi.local")
        #expect(result != nil)
    }

    @Test func refusesToEvaluateForAnotherHost() {
        #expect(evaluator.evaluate(Self.trust(for: "test-leaf"), host: "192.168.1.20")
                == .unexpectedHost("192.168.1.20"))
    }

    @Test func rejectsAnExpiredLeaf() {
        var late = evaluator
        late.verifyDate = ISO8601DateFormatter().date(from: "2031-01-01T00:00:00Z")!
        #expect(late.evaluate(Self.trust(for: "test-leaf"), host: "woodland-pi.local") != nil)
    }

    @Test func caLoaderChecksTheFingerprint() {
        let der = Fixture.data("test-ca")
        #expect(throws: CertificateAuthorityError.fingerprintMismatch(Self.fingerprint(der))) {
            try PinnedCertificateAuthority.certificate(der: der, expectedSHA256: String(repeating: "0", count: 64))
        }
    }

    @Test func caLoaderReportsAMissingResource() {
        let configuration = GatewayConfiguration(host: "woodland-pi.local", caSHA256: "",
                                                 caResourceName: "DoesNotExist")
        #expect(throws: CertificateAuthorityError.missing("DoesNotExist.der")) {
            try PinnedCertificateAuthority.load(configuration, bundle: Bundle(for: BundleToken.self))
        }
    }

    @Test func woodlandConfigurationPinsTheWoodlandCA() {
        #expect(GatewayConfiguration.woodland.host == "woodland-pi.local")
        #expect(GatewayConfiguration.woodland.baseURL.absoluteString == "https://woodland-pi.local")
        #expect(GatewayConfiguration.woodland.caSHA256.count == 64)
    }

    private final class BundleToken {}
}

@Suite struct TransportErrorTests {
    @Test(arguments: [URLError.Code.cannotFindHost, .dnsLookupFailed, .cannotConnectToHost,
                      .notConnectedToInternet])
    func failuresBeforeTheRequestAreUnreachable(code: URLError.Code) {
        let error = TransportError(URLError(code), trustFailure: nil)
        #expect(!error.mayHaveReachedServer)
        if case .unreachable = error {} else { Issue.record("expected unreachable, got \(error)") }
    }

    @Test(arguments: [URLError.Code.serverCertificateUntrusted, .serverCertificateHasBadDate,
                      .serverCertificateHasUnknownRoot, .serverCertificateNotYetValid])
    func certificateFailuresAreTLSRejections(code: URLError.Code) {
        let error = TransportError(URLError(code), trustFailure: nil)
        #expect(!error.mayHaveReachedServer)
        if case .tlsRejected = error {} else { Issue.record("expected tlsRejected, got \(error)") }
    }

    @Test func ourOwnTrustRejectionIsReportedAsTLSNotCancellation() {
        let error = TransportError(URLError(.cancelled), trustFailure: .rejected("wrong CA"))
        #expect(error == .tlsRejected("wrong CA"))
        #expect(!error.mayHaveReachedServer)
    }

    /// Anything that can happen after request bytes left the phone is uncertain.
    @Test(arguments: [URLError.Code.timedOut, .networkConnectionLost, .cancelled,
                      .secureConnectionFailed, .badServerResponse, .unknown])
    func possiblyDeliveredFailuresAreUncertain(code: URLError.Code) {
        #expect(TransportError(URLError(code), trustFailure: nil).mayHaveReachedServer)
    }
}
