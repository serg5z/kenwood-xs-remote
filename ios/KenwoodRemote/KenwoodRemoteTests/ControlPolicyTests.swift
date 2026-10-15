import Foundation
import Testing
@testable import KenwoodRemote

/// Every command's exact request: path, body bytes and client timeout.
@Suite struct ControlCommandTests {
    @Test(arguments: [
        (ControlCommand.powerToggle(expected: nil), "/api/v1/power", #"{"action":"TOGGLE"}"#, 15.0),
        (.powerToggle(expected: "ON"), "/api/v1/power", #"{"action":"TOGGLE","expected":"ON"}"#, 15.0),
        (.powerToggle(expected: "OFF_INFERRED"), "/api/v1/power",
         #"{"action":"TOGGLE","expected":"OFF_INFERRED"}"#, 15.0),
        (.power(on: true), "/api/v1/power", #"{"action":"ON"}"#, 6.0),
        (.power(on: false), "/api/v1/power", #"{"action":"OFF"}"#, 6.0),
        (.source(.cd), "/api/v1/source", #"{"source":"CD"}"#, 12.0),
        (.source(.video1), "/api/v1/source", #"{"source":"VIDEO1"}"#, 12.0),
        (.source(.tape1), "/api/v1/source", #"{"source":"TAPE1"}"#, 12.0),
        (.tape2Monitor, "/api/v1/receiver/monitor", "{}", 12.0),
        (.mute, "/api/v1/mute", "{}", 6.0),
        (.receiverQuery, "/api/v1/receiver/query", "{}", 6.0),
        (.volumeStep(up: true), "/api/v1/volume/step", #"{"direction":"UP"}"#, 6.0),
        (.volumeStep(up: false), "/api/v1/volume/step", #"{"direction":"DOWN"}"#, 6.0),
        (.volumeHold(.start(direction: "UP")), "/api/v1/volume/hold", #"{"action":"START","direction":"UP"}"#, 4.0),
        (.volumeHold(.heartbeat(holdId: "AbC-_0123456789abcdefg")), "/api/v1/volume/hold",
         #"{"action":"HEARTBEAT","hold_id":"AbC-_0123456789abcdefg"}"#, 4.0),
        (.volumeHold(.release(holdId: "AbC-_0123456789abcdefg")), "/api/v1/volume/hold",
         #"{"action":"RELEASE","hold_id":"AbC-_0123456789abcdefg"}"#, 4.0),
        (.volumeHold(.release(holdId: nil)), "/api/v1/volume/hold", #"{"action":"RELEASE"}"#, 4.0),
        (.cdSeek(.start(direction: "FORWARD")), "/api/v1/cd/seek", #"{"action":"START","direction":"FORWARD"}"#, 4.0),
        (.cdSeek(.start(direction: "BACKWARD")), "/api/v1/cd/seek", #"{"action":"START","direction":"BACKWARD"}"#, 4.0),
        (.cdTrack(next: true), "/api/v1/cd/track", #"{"action":"NEXT"}"#, 6.0),
        (.cdTrack(next: false), "/api/v1/cd/track", #"{"action":"PREVIOUS"}"#, 6.0),
        (.cdPlayback(.playPause), "/api/v1/cd/playback", #"{"action":"PLAY_PAUSE"}"#, 6.0),
        (.cdPlayback(.stop), "/api/v1/cd/playback", #"{"action":"STOP"}"#, 6.0),
        (.cdPlayback(.timeDisplay), "/api/v1/cd/playback", #"{"action":"TIME_DISPLAY"}"#, 6.0),
        (.cdPlayback(.repeatMode), "/api/v1/cd/playback", #"{"action":"REPEAT"}"#, 6.0),
        (.cdPlayback(.discSkip), "/api/v1/cd/playback", #"{"action":"DISC_SKIP"}"#, 6.0),
        (.cdTrackSelect(1), "/api/v1/cd/track/select", #"{"track":1}"#, 12.0),
        (.cdTrackSelect(39), "/api/v1/cd/track/select", #"{"track":39}"#, 12.0),
    ])
    func requestFormat(command: ControlCommand, path: String, body: String, timeout: Double) throws {
        #expect(command.path == path)
        let data = try #require(command.body)
        #expect(String(decoding: data, as: UTF8.self) == body)
        #expect(data.count <= 64)
        #expect(command.timeout == timeout)
        // Every body is valid JSON with the Gateway's exact key set.
        #expect(try JSONSerialization.jsonObject(with: data) is [String: Any])
    }

    @Test(arguments: [
        ControlCommand.cdTrackSelect(0), .cdTrackSelect(40), .cdTrackSelect(-1),
        .powerToggle(expected: "UNKNOWN"),
        .volumeHold(.start(direction: "FORWARD")), .cdSeek(.start(direction: "UP")),
        .volumeHold(.heartbeat(holdId: "")), .volumeHold(.heartbeat(holdId: #"x","action":"START"#)),
        .cdSeek(.release(holdId: "has space")), .cdSeek(.heartbeat(holdId: "ünïcode")),
        // Valid by the Gateway's rule but over its 64-byte body limit.
        .cdSeek(.heartbeat(holdId: String(repeating: "a", count: 43))),
    ])
    func invalidCommandsHaveNoBody(command: ControlCommand) {
        #expect(command.body == nil)
    }
}

/// The agreed outcome policy, case by case.
@Suite struct ControlOutcomePolicyTests {
    static func reply(_ json: String) -> ControlReply {
        try! JSONDecoder().decode(ControlReply.self, from: Data(json.utf8))
    }

    @Test func http200IsTransmissionOnly() {
        let outcome = ControlOutcome(.response(status: 200, reply: Self.reply(#"{"result":"SERVICE_TX_OK"}"#),
                                               gatewayJSON: true))
        #expect(outcome == .txVerified("SERVICE_TX_OK"))
    }

    @Test func http200WithoutAReadableBodyIsUncertain() {
        #expect(ControlOutcome(.response(status: 200, reply: nil, gatewayJSON: false)) == .uncertain("UNREADABLE REPLY"))
    }

    @Test func invalidOriginIsNothingSent() {
        let outcome = ControlOutcome(.response(status: 403, reply: Self.reply(#"{"error":"HTTPS Origin required"}"#),
                                               gatewayJSON: true))
        #expect(outcome == .nothingSent("ORIGIN REJECTED"))
    }

    @Test func invalidCSRFTokenIsUncertain() {
        let outcome = ControlOutcome(.response(status: 403, reply: Self.reply(#"{"error":"Invalid CSRF token"}"#),
                                               gatewayJSON: true))
        #expect(outcome == .uncertain("INVALID CSRF TOKEN"))
    }

    @Test func unexplained403IsUncertain() {
        #expect(ControlOutcome(.response(status: 403, reply: nil, gatewayJSON: false)) == .uncertain("INVALID CSRF TOKEN"))
    }

    @Test(arguments: [502, 504])
    func gatewayErrorsAreUncertain(status: Int) {
        #expect(ControlOutcome(.response(status: status, reply: nil, gatewayJSON: false)) == .uncertain("HTTP \(status)"))
        let json = Self.reply(#"{"result":"TIMEOUT","physical_check_required":true}"#)
        #expect(ControlOutcome(.response(status: status, reply: json, gatewayJSON: true)) == .uncertain("TIMEOUT"))
    }

    @Test func serviceUncertainIsUncertain() {
        let reply = Self.reply(#"{"result":"SERVICE_UNCERTAIN","physical_check_required":true}"#)
        #expect(ControlOutcome(.response(status: 502, reply: reply, gatewayJSON: true)) == .uncertain("SERVICE_UNCERTAIN"))
    }

    @Test(arguments: ["FAULT_LATCHED", "CD_FAULT_LATCHED"])
    func faultsAreFaults(result: String) {
        let reply = Self.reply(#"{"result":"\#(result)","physical_check_required":true}"#)
        #expect(ControlOutcome(.response(status: 409, reply: reply, gatewayJSON: true)) == .fault(result))
    }

    @Test(arguments: ["BUSY", "CD_BUSY", "VOLUME_BUSY", "NOT_ELIGIBLE", "REJECTED", "TOGGLE_UNAVAILABLE"])
    func cleanRefusalsAreNothingSent(result: String) {
        let reply = Self.reply(#"{"result":"\#(result)","physical_check_required":false}"#)
        #expect(ControlOutcome(.response(status: 409, reply: reply, gatewayJSON: true)) == .nothingSent(result))
    }

    @Test func refusalThatMayHaveMovedHardwareIsUncertain() {
        // Leaving CD: SELECT_SOURCE verified, then CD_STOP refused.
        let reply = Self.reply(#"{"result":"REJECTED","stage":"CD_STOP","physical_check_required":true}"#)
        #expect(ControlOutcome(.response(status: 409, reply: reply, gatewayJSON: true)) == .uncertain("REJECTED"))
    }

    @Test(arguments: ["POWER_STATE_UNKNOWN", "STATE_CHANGED", "INTERVENING_TX"])
    func refusedToggleIsNothingSent(result: String) {
        let reply = Self.reply(#"{"result":"\#(result)","power_sent":false,"physical_check_required":false}"#)
        #expect(ControlOutcome(.response(status: 409, reply: reply, gatewayJSON: true)) == .nothingSent(result))
    }

    @Test func aSentPowerFrameIsNeverNothingSent() {
        let reply = Self.reply(#"{"result":"REJECTED","power_sent":true}"#)
        #expect(ControlOutcome(.response(status: 409, reply: reply, gatewayJSON: true)) == .uncertain("REJECTED"))
    }

    @Test func otherCleanRefusals() {
        #expect(ControlOutcome(.response(status: 429, reply: Self.reply(#"{"result":"TOO_FAST"}"#), gatewayJSON: true))
                == .nothingSent("TOO FAST"))
        #expect(ControlOutcome(.response(status: 400, reply: nil, gatewayJSON: true)) == .nothingSent("REQUEST REJECTED"))
        #expect(ControlOutcome(.response(status: 501, reply: nil, gatewayJSON: true)) == .nothingSent("CONTROL DISABLED"))
        #expect(ControlOutcome(.response(status: 401, reply: nil, gatewayJSON: true)) == .nothingSent("SESSION EXPIRED"))
        #expect(ControlOutcome(.response(status: 401, reply: nil, gatewayJSON: false)) == .nothingSent("PASSWORD REJECTED"))
        #expect(ControlOutcome(.response(status: 503, reply: Self.reply(#"{"result":"UNAVAILABLE"}"#), gatewayJSON: true))
                == .nothingSent("UNAVAILABLE"))
    }

    @Test func nonGateway503IsUncertain() {
        #expect(ControlOutcome(.response(status: 503, reply: nil, gatewayJSON: false)) == .uncertain("HTTP 503"))
    }

    @Test func transportFailures() {
        #expect(ControlOutcome(.transportFailed(.unreachable("x"))) == .nothingSent("PI NOT REACHED"))
        #expect(ControlOutcome(.transportFailed(.tlsRejected("x"))) == .nothingSent("TLS REJECTED"))
        #expect(ControlOutcome(.transportFailed(.timedOut)) == .uncertain("NO REPLY (TIMEOUT)"))
        #expect(ControlOutcome(.transportFailed(.connectionLost)) == .uncertain("CONNECTION LOST"))
        #expect(ControlOutcome(.transportFailed(.cancelled)) == .uncertain("REQUEST CANCELLED"))
        #expect(ControlOutcome(.transportFailed(.other("x"))) == .uncertain("NETWORK ERROR"))
    }

    @Test func notAttemptedIsNothingSent() {
        #expect(ControlOutcome(.notAttempted("CANCELLED BEFORE SEND")) == .nothingSent("CANCELLED BEFORE SEND"))
    }
}
