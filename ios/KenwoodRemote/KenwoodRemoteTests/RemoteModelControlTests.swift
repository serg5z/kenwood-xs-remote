import Foundation
import Testing
@testable import KenwoodRemote

@MainActor
final class TestClock {
    var date = Date(timeIntervalSince1970: 1_800_000_000)
    func advance(ms: Int) { date += Double(ms) / 1000 }
}

@MainActor
final class BackgroundTaskLog {
    var began: [String] = []
    var ended = 0
}

@MainActor
struct ModelHarness {
    let transport = StubTransport()
    let clock = TestClock()
    let background = BackgroundTaskLog()
    let model: RemoteModel

    init(capabilities: Data = Fixture.data("capabilities"), status: Data = Fixture.data("status-connected")) {
        transport.setHealthyReadDefaults(capabilities: capabilities, status: status)
        let transport = transport
        let clock = clock
        let background = background
        model = RemoteModel(credentialStore: MemoryCredentialStore(RemoteModelTests.credentials),
                            now: { clock.date },
                            beginBackgroundTask: { name in
                                background.began.append(name)
                                return { background.ended += 1 }
                            }) { _ in
            GatewayClient(baseURL: URL(string: "https://woodland-pi.local")!, transport: transport)
        }
    }

    func online() async -> RemoteModel {
        await model.refresh()
        return model
    }
}

@MainActor @Suite struct RemoteModelControlTests {
    static let ok = #"{"result":"SERVICE_TX_OK","receiver_execution":"UNVERIFIED"}"#

    @Test func everyAdvertisedControlIsEnabledWhenOnline() async {
        let model = await ModelHarness().online()
        let controls: [Control] = [.power, .tape2Monitor, .mute, .sync, .volumeStep(up: true), .volumeHold(up: false),
                                   .cdTrack(next: true), .cdTrack(next: false), .cdSeek(forward: true),
                                   .cdTrackSelect] + Source.allCases.map { .source($0) }
                                  + PlaybackAction.allCases.map { .cdPlayback($0) }
        for control in controls { #expect(model.isEnabled(control), "\(control)") }
    }

    @Test func nothingIsEnabledOffline() async {
        let harness = ModelHarness()
        #expect(!harness.model.isEnabled(.mute))
        harness.model.mute()
        await harness.model.waitUntilIdle()
        #expect(harness.transport.posts.isEmpty)
    }

    @Test func disabledCapabilitiesAreNeverSent() async {
        let harness = ModelHarness(capabilities: Fixture.capabilities("mute", ["enabled": false]))
        let model = await harness.online()
        #expect(!model.isEnabled(.mute))
        model.mute()
        await model.waitUntilIdle()
        #expect(harness.transport.posts.isEmpty)
    }

    @Test func unadvertisedActionsAreDisabled() async {
        let harness = ModelHarness(capabilities: Fixture.capabilities("cd_playback",
                                                                      ["available_actions": ["PLAY_PAUSE", "STOP"]]))
        let model = await harness.online()
        #expect(model.isEnabled(.cdPlayback(.stop)))
        #expect(!model.isEnabled(.cdPlayback(.repeatMode)))
        model.cdPlayback(.repeatMode)
        await model.waitUntilIdle()
        #expect(harness.transport.posts.isEmpty)
    }

    @Test func duplicateTapsSendOnce() async {
        let harness = ModelHarness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/mute", .json(200, Self.ok, delayMs: 50))
        model.mute()
        model.mute()
        model.mute()
        #expect(model.busy)
        await model.waitUntilIdle()
        #expect(harness.transport.posts(to: "/api/v1/mute").count == 1)
        #expect(model.lastResult == ResultLine(text: "MUTE TX VERIFIED · RECEIVER ACTION UNVERIFIED", tone: .verified))
    }

    @Test func otherControlsAreDroppedWhileOneRuns() async {
        let harness = ModelHarness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/cd/track", .json(200, Self.ok, delayMs: 50))
        model.cdTrack(next: true)
        model.volumeStep(up: true)
        model.selectSource(.tuner)
        await model.waitUntilIdle()
        #expect(harness.transport.posts.map(\.url!.path) == ["/api/v1/cd/track"])
    }

    @Test func volumeSpacingDropsEarlyTaps() async {
        let harness = ModelHarness()
        let model = await harness.online()
        harness.transport.setDefault(path: "/api/v1/volume/step", .json(200, Self.ok))
        model.volumeStep(up: true)
        await model.waitUntilIdle()
        harness.clock.advance(ms: 100)
        model.volumeStep(up: true)          // inside 250 ms: dropped
        await model.waitUntilIdle()
        harness.clock.advance(ms: 200)
        model.volumeStep(up: false)         // 300 ms later: allowed
        await model.waitUntilIdle()
        #expect(harness.transport.posts(to: "/api/v1/volume/step") == [#"{"direction":"UP"}"#, #"{"direction":"DOWN"}"#])
    }

    @Test func zeroCDIntervalIsHonouredAsZero() async {
        let harness = ModelHarness()
        let model = await harness.online()
        #expect(model.cdIntervalMs == 0)
        harness.transport.setDefault(path: "/api/v1/cd/track", .json(200, Self.ok))
        model.cdTrack(next: false)
        await model.waitUntilIdle()
        model.cdTrack(next: false)
        await model.waitUntilIdle()
        #expect(harness.transport.posts(to: "/api/v1/cd/track").count == 2)
    }

    @Test func invalidCDIntervalFallsBackToOneSecond() async {
        let harness = ModelHarness(capabilities: Fixture.capabilities("cd_transport", ["min_interval_ms": 50]))
        let model = await harness.online()
        #expect(model.cdIntervalMs == 1000)
    }

    @Test func refusedRequestsDoNotStartClientSpacing() async {
        let harness = ModelHarness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/volume/step", .json(409, #"{"result":"VOLUME_BUSY"}"#))
        harness.transport.enqueue(path: "/api/v1/volume/step", .json(200, Self.ok))
        model.volumeStep(up: true)
        await model.waitUntilIdle()
        #expect(model.lastResult?.text == "VOL+ VOLUME BUSY · NOTHING SENT")
        model.volumeStep(up: true)
        await model.waitUntilIdle()
        #expect(harness.transport.posts(to: "/api/v1/volume/step").count == 2)
    }

    @Test func uncertainOutcomesAreReportedAndNeverRetried() async {
        let harness = ModelHarness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/cd/playback", .html(504))
        model.cdPlayback(.playPause)
        await model.waitUntilIdle()
        #expect(harness.transport.posts.count == 1)
        #expect(model.lastResult == ResultLine(text: "PLAY/PAUSE HTTP 504 · UNCERTAIN · CHECK CHANGER", tone: .uncertain))
    }

    @Test func invalidCSRFIsUncertainInTheUI() async {
        let harness = ModelHarness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/mute", .json(403, #"{"error":"Invalid CSRF token"}"#))
        model.mute()
        await model.waitUntilIdle()
        #expect(model.lastResult?.tone == .uncertain)
        #expect(model.lastResult?.text == "MUTE INVALID CSRF TOKEN · UNCERTAIN · CHECK RECEIVER")
        #expect(harness.transport.posts.count == 1)
    }

    @Test func invalidOriginIsNothingSentInTheUI() async {
        let harness = ModelHarness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/mute", .json(403, #"{"error":"HTTPS Origin required"}"#))
        model.mute()
        await model.waitUntilIdle()
        #expect(model.lastResult == ResultLine(text: "MUTE ORIGIN REJECTED · NOTHING SENT", tone: .nothingSent))
    }

    @Test func networkFailuresAreClassified() async {
        let harness = ModelHarness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/mute", .failure(.unreachable("down")))
        model.mute()
        await model.waitUntilIdle()
        #expect(model.lastResult?.text == "MUTE PI NOT REACHED · NOTHING SENT")
        harness.transport.enqueue(path: "/api/v1/mute", .failure(.connectionLost))
        model.mute()
        await model.waitUntilIdle()
        #expect(model.lastResult?.text == "MUTE CONNECTION LOST · UNCERTAIN · CHECK RECEIVER")
    }

    @Test func aCDFaultStopsCDControlsUntilTheNextRead() async {
        let harness = ModelHarness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/cd/track",
                                  .json(409, #"{"result":"CD_FAULT_LATCHED","physical_check_required":true}"#))
        // The read after the control fails, so only the local latch applies.
        harness.transport.enqueue(.status, .failure(.timedOut))
        model.cdTrack(next: true)
        await model.waitUntilIdle()
        #expect(model.lastResult == ResultLine(text: "CD FAULT LATCHED · STOP AND INSPECT CHANGER", tone: .fault))
        #expect(model.localFault == .cd)
    }

    @Test func serviceFaultsShowAsBanners() async {
        let model = await ModelHarness(status: Fixture.status(["cd_state": "FAULT_LATCHED"])).online()
        #expect(model.faultBanner == "CD FAULT LATCHED · STOP AND INSPECT CHANGER")
        let other = await ModelHarness(status: Fixture.status(["state": "FAULT_LATCHED"])).online()
        #expect(other.faultBanner == "XS8 FAULT LATCHED · CHECK RECEIVER · RESTART REQUIRED")
    }

    @Test func powerToggleSendsTheFreshObservationAsExpected() async {
        let harness = ModelHarness()
        let model = await harness.online()
        #expect(model.powerIndicator == .on)
        harness.transport.enqueue(path: "/api/v1/power", .json(200,
            #"{"result":"SERVICE_TX_OK","action":"TOGGLE","sent":"OFF","power_sent":true,"decision":{"state":"ON"}}"#))
        model.tapPower()
        await model.waitUntilIdle()
        #expect(harness.transport.posts(to: "/api/v1/power") == [#"{"action":"TOGGLE","expected":"ON"}"#])
        #expect(model.lastResult?.text == "POWER OFF TX VERIFIED · RECEIVER WAS ON · RECEIVER POWER UNVERIFIED")
        // The displayed power state comes only from the next observation.
        #expect(model.powerIndicator == .on)
    }

    @Test func refusedToggleOffersTheExplicitChoiceWithoutRetrying() async {
        let harness = ModelHarness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/power", .json(409,
            #"{"result":"POWER_STATE_UNKNOWN","power_sent":false,"physical_check_required":false}"#))
        model.tapPower()
        await model.waitUntilIdle()
        #expect(harness.transport.posts.count == 1)
        #expect(model.showingPowerChooser)
        #expect(model.lastResult == ResultLine(text: "POWER TOGGLE POWER STATE UNKNOWN · NOTHING SENT · CHOOSE ON OR OFF",
                                               tone: .nothingSent))
        harness.transport.enqueue(path: "/api/v1/power", .json(200, #"{"result":"SERVICE_TX_OK","action":"ON"}"#))
        model.choosePower(on: true)
        await model.waitUntilIdle()
        #expect(harness.transport.posts(to: "/api/v1/power").last == #"{"action":"ON"}"#)
    }

    @Test func withoutToggleThePowerKeyOpensTheChooser() async {
        let harness = ModelHarness(capabilities: Fixture.capabilities("power", ["toggle_available": false]))
        let model = await harness.online()
        model.tapPower()
        #expect(model.showingPowerChooser)
        #expect(harness.transport.posts.isEmpty)
    }

    @Test func sourceSelectionWaitsForAnObservationAfterIt() async {
        let harness = ModelHarness()
        let model = await harness.online()
        #expect(model.sourceIndicator.highlight(for: .cd) == .fresh)
        harness.transport.enqueue(path: "/api/v1/source", .json(200, """
            {"result":"SERVICE_TX_OK","gpio_tx":"TX_OK","waveform":"MATCH","receiver_execution":"UNVERIFIED",
             "sync":{"since_mono_ns":9000000000000000,"result":"OBSERVATION_RECORDED","observed_source":"TUNER",
                     "observed_monitor":"OFF","requested":"TUNER","match":true}}
            """))
        model.selectSource(.tuner)
        await model.waitUntilIdle()
        #expect(model.lastResult == ResultLine(text: "TUNER TX VERIFIED · RECEIVER REPORTS TUNER", tone: .verified))
        // The refreshed STATUS still holds the older CD observation: not shown as current.
        #expect(model.sourceIndicator.quality == .awaitingObservation)
        #expect(Source.allCases.allSatisfy { model.sourceIndicator.highlight(for: $0) == .none })
    }

    @Test func sourceMismatchIsFlagged() async {
        let harness = ModelHarness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/source", .json(200, """
            {"result":"SERVICE_TX_OK","sync":{"since_mono_ns":1,"result":"OBSERVATION_RECORDED",
             "observed_source":"CD","match":false}}
            """))
        model.selectSource(.phono)
        await model.waitUntilIdle()
        #expect(model.lastResult == ResultLine(text: "PHONO TX VERIFIED · RECEIVER REPORTS CD · CHECK RECEIVER",
                                               tone: .uncertain))
    }

    @Test func leavingCDReportsTheStop() async {
        let harness = ModelHarness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/source", .json(200, #"{"result":"SERVICE_TX_OK","cd_stop":{"gpio_tx":"TX_OK"}}"#))
        model.selectSource(.video1)
        await model.waitUntilIdle()
        #expect(model.lastResult?.text == "VIDEO1 TX VERIFIED · CD STOP TX VERIFIED · SELECTION UNVERIFIED")
    }

    @Test func sourceSpacingIsOneSecond() async {
        let harness = ModelHarness()
        let model = await harness.online()
        harness.transport.setDefault(path: "/api/v1/source", .json(200, Self.ok))
        model.selectSource(.cd)
        await model.waitUntilIdle()
        harness.clock.advance(ms: 500)
        model.selectSource(.tuner)
        await model.waitUntilIdle()
        harness.clock.advance(ms: 600)
        model.selectSource(.tuner)
        await model.waitUntilIdle()
        #expect(harness.transport.posts(to: "/api/v1/source") == [#"{"source":"CD"}"#, #"{"source":"TUNER"}"#])
    }

    @Test func syncIsExplicitAndSpaced() async {
        let harness = ModelHarness()
        let model = await harness.online()
        await model.refresh()
        #expect(harness.transport.posts.isEmpty)   // reads never query
        harness.transport.setDefault(path: "/api/v1/receiver/query", .json(200,
            #"{"result":"OBSERVATION_RECORDED","observed_source":"CD","observed_monitor":"OFF"}"#))
        model.sync()
        await model.waitUntilIdle()
        model.sync()                                // within 2 s: dropped
        await model.waitUntilIdle()
        #expect(harness.transport.posts(to: "/api/v1/receiver/query") == ["{}"])
        #expect(model.lastResult?.text == "SYNC RECORDED · RECEIVER REPORTS CD · MONITOR OFF")
    }

    @Test func monitorToggleReportsTheReceiverNotTheTap() async {
        let harness = ModelHarness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/receiver/monitor", .json(200, """
            {"result":"SERVICE_TX_OK","mapping":"PROVISIONAL","sync":{"since_mono_ns":9000000000000000,
             "result":"OBSERVATION_RECORDED","observed_monitor":"ON","expected_monitor":null,"match":null}}
            """))
        model.toggleMonitor()
        await model.waitUntilIdle()
        #expect(model.lastResult?.text == "TAPE2 MONITOR TX VERIFIED · RECEIVER REPORTS MONITOR ON")
        #expect(model.monitorIndicator == .unknown)
    }

    @Test func trackSelectSendsOnlyValidNumbers() async {
        let harness = ModelHarness()
        let model = await harness.online()
        #expect(model.validTrack("12") == 12)
        #expect(model.validTrack("0") == nil)
        #expect(model.validTrack("07") == nil)
        #expect(model.validTrack("40") == nil)
        #expect(model.validTrack("1a") == nil)
        harness.transport.enqueue(path: "/api/v1/cd/track/select", .json(200,
            #"{"result":"SERVICE_TX_OK","action":"SELECT","track":12,"frames":2}"#))
        model.selectTrack(12)
        await model.waitUntilIdle()
        model.selectTrack(40)
        await model.waitUntilIdle()
        #expect(harness.transport.posts(to: "/api/v1/cd/track/select") == [#"{"track":12}"#])
        #expect(model.lastResult?.text == "TRACK 12 TX VERIFIED · TRACK SELECTION UNVERIFIED")
    }

    @Test func narrowerAdvertisedTrackRangeIsRespected() async {
        let model = await ModelHarness(capabilities: Fixture.capabilities("cd_track_select", ["max_track": 20])).online()
        #expect(model.trackRange == 1...20)
        #expect(model.validTrack("21") == nil)
    }

    @Test func aControlRefreshesObservedStateAfterwards() async {
        let harness = ModelHarness()
        let model = await harness.online()
        let readsBefore = harness.transport.paths.filter { $0 == "/api/v1/status" }.count
        harness.transport.enqueue(path: "/api/v1/mute", .json(200, Self.ok))
        model.mute()
        await model.waitUntilIdle()
        #expect(harness.transport.paths.filter { $0 == "/api/v1/status" }.count == readsBefore + 1)
        #expect(harness.transport.paths.last == "/api/v1/capabilities")
    }
}
