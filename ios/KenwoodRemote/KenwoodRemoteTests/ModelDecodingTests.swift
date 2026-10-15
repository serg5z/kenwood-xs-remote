import Foundation
import Testing
@testable import KenwoodRemote

/// Fixtures were produced by the Gateway's own capabilities(), receiver_state()
/// and PowerObservation.snapshot() (xs8_gateway.py), so these decode real shapes.
@Suite struct ModelDecodingTests {
    @Test func decodesConnectedStatus() throws {
        let status = try JSONDecoder().decode(StatusResponse.self, from: Fixture.data("status-connected"))
        #expect(status.serviceConnected)
        #expect(status.status?["cd_state"] == "IDLE")
        #expect(status.receiver?.source.value == "CD")
        #expect(status.receiver?.source.freshness == "FRESH")
        #expect(status.receiver?.monitor.freshness == "LAST_KNOWN")
        #expect(status.receiver?.integrity == "HEALTHY")
        #expect(status.power?.value == "ON")
        #expect(status.power?.freshness == "FRESH")
        #expect(status.cdChanger == nil)
    }

    @Test func decodesUnavailableStatus() throws {
        let status = try JSONDecoder().decode(StatusResponse.self, from: Fixture.data("status-unavailable"))
        #expect(!status.serviceConnected)
        #expect(status.receiver == nil)
    }

    @Test func sessionAvailabilityCarriesNoToken() throws {
        let session = try JSONDecoder().decode(SessionAvailability.self, from: Fixture.data("session"))
        #expect(session.cdTrackSelect && session.power && session.volumeHold)
        #expect(!String(describing: session).contains("token-from-session"))
    }

    @Test func missingSessionFlagsAreFalse() throws {
        let session = try JSONDecoder().decode(SessionAvailability.self,
                                               from: Data(#"{"csrf_token":"x","mute_available":true}"#.utf8))
        #expect(session.mute)
        #expect(!session.cdTrackSelect)
    }

    @Test func decodesEveryCapabilityOperation() throws {
        let capabilities = try JSONDecoder().decode(Capabilities.self, from: Fixture.data("capabilities"))
        #expect(capabilities.apiVersion == "v0.1")
        for name in OperationName.allCases {
            #expect(capabilities.operation(name) != nil, "missing \(name.rawValue)")
        }
        let select = try #require(capabilities.operation(.cdTrackSelect))
        #expect(select.minTrack == 1 && select.maxTrack == 39 && select.frameGapMs == 200)
        #expect(capabilities.operation(.cdPlayback)?.availableActions ==
                ["PLAY_PAUSE", "STOP", "TIME_DISPLAY", "REPEAT", "DISC_SKIP"])
        #expect(capabilities.operation(.power)?.toggleAvailable == true)
        #expect(capabilities.operation(.volumeHold)?.maxHoldMs == 20000)
        #expect(capabilities.operation(.source)?.availableSources ==
                ["CD", "TUNER", "PHONO", "VIDEO1", "VIDEO2", "TAPE1"])
        // The live Gateway runs the experimental 0 ms CD interval; 0 must decode as 0.
        #expect(capabilities.operation(.cdTransport)?.minIntervalMs == 0)
    }

    @Test func unknownOperationsAndFieldsAreIgnored() throws {
        let json = #"{"api_version":"v0.2","operations":{"future_op":{"enabled":true,"new_field":[1]},"mute":{}}}"#
        let capabilities = try JSONDecoder().decode(Capabilities.self, from: Data(json.utf8))
        #expect(capabilities.operation(.mute)?.enabled == false)
        #expect(capabilities.operations["future_op"]?.enabled == true)
    }
}
