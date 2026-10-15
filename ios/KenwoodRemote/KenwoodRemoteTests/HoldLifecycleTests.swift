import Foundation
import Testing
@testable import KenwoodRemote

/// Press-and-hold against a scripted Gateway, with short real-time timings.
@MainActor @Suite(.serialized) struct HoldLifecycleTests {
    nonisolated static let holdId = "hold-id-0123456789abcd"
    nonisolated static func started(_ result: String = "HOLD_STARTED", maxHoldMs: Int = 20000, delayMs: Int = 0) -> StubTransport.Reply {
        .json(200, #"{"result":"\#(result)","direction":"UP","hold_id":"\#(holdId)","heartbeat_ms":50,"#
              + #""lease_ms":1200,"max_hold_ms":\#(maxHoldMs),"csrf_token":"after-start"}"#, delayMs: delayMs)
    }
    nonisolated static let renewed = StubTransport.Reply.json(200, #"{"result":"LEASE_RENEWED","csrf_token":"after-beat"}"#)
    nonisolated static let released = StubTransport.Reply.json(200,
        #"{"result":"RELEASED","gpio_tx":"TX_OK","waveform":"MATCH","csrf_token":"after-release"}"#)

    /// Hold decision after 150 ms instead of 400 ms.
    static func harness() -> ModelHarness {
        ModelHarness(capabilities: Fixture.capabilities("volume_hold", ["hold_delay_ms": 150]))
    }

    /// START, HEARTBEAT and RELEASE each get their normal answer.
    static func respondNormally(_ harness: ModelHarness, path: String, started: String = "HOLD_STARTED") {
        harness.transport.setResponder(path: path) { body in
            body.contains("START") ? Self.started(started) : body.contains("HEARTBEAT") ? Self.renewed : Self.released
        }
    }

    static func actions(_ bodies: [String]) -> [String] {
        bodies.compactMap { body in
            (try? JSONSerialization.jsonObject(with: Data(body.utf8)) as? [String: String])?["action"]
        }
    }

    @Test func aShortPressIsOneStepAndNoHold() async {
        let harness = Self.harness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/volume/step", .json(200, #"{"result":"SERVICE_TX_OK"}"#))
        model.pressBegan(.volumeUp)
        model.pressEnded(.volumeUp, completed: true)
        await model.waitUntilIdle()
        try? await Task.sleep(for: .milliseconds(200))
        #expect(harness.transport.posts(to: "/api/v1/volume/step") == [#"{"direction":"UP"}"#])
        #expect(harness.transport.posts(to: "/api/v1/volume/hold").isEmpty)
    }

    @Test func aCancelledTouchSendsNothing() async {
        let harness = Self.harness()
        let model = await harness.online()
        model.pressBegan(.volumeDown)
        model.pressEnded(.volumeDown, completed: false)
        try? await Task.sleep(for: .milliseconds(200))
        await model.waitUntilIdle()
        #expect(harness.transport.posts.isEmpty)
    }

    @Test func aLongPressHoldsRenewsAndReleasesOnce() async {
        let harness = Self.harness()
        let model = await harness.online()
        Self.respondNormally(harness, path: "/api/v1/volume/hold")
        model.pressBegan(.volumeUp)
        try? await Task.sleep(for: .milliseconds(400))
        #expect(model.activeHold?.key == .volumeUp)
        // The release waits behind whatever heartbeat is in flight.
        model.pressEnded(.volumeUp, completed: true)
        await model.waitUntilIdle()

        let bodies = harness.transport.posts(to: "/api/v1/volume/hold")
        let actions = Self.actions(bodies)
        #expect(actions.first == "START")
        #expect(actions.filter { $0 == "HEARTBEAT" }.count >= 2)
        #expect(actions.last == "RELEASE")
        #expect(actions.filter { $0 == "RELEASE" }.count == 1)
        #expect(bodies.last == #"{"action":"RELEASE","hold_id":"\#(Self.holdId)"}"#)
        // A gesture is a hold or a step, never both.
        #expect(harness.transport.posts(to: "/api/v1/volume/step").isEmpty)
        #expect(model.lastResult?.text == "VOL+ RELEASE TX VERIFIED · VOLUME CHANGE UNVERIFIED")
        #expect(model.activeHold == nil && !model.busy)
    }

    @Test func heartbeatsUseTheSuccessorTokens() async {
        let harness = Self.harness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/cd/seek", Self.started("SEEK_STARTED"))
        harness.transport.enqueue(path: "/api/v1/cd/seek", Self.renewed)
        harness.transport.enqueue(path: "/api/v1/cd/seek", Self.released)
        model.pressBegan(.seekForward)
        try? await Task.sleep(for: .milliseconds(80))
        model.pressEnded(.seekForward, completed: true)
        await model.waitUntilIdle()
        let tokens = harness.transport.posts.filter { $0.url!.path == "/api/v1/cd/seek" }
            .map { $0.value(forHTTPHeaderField: "X-CSRF-Token") }
        #expect(tokens.first == "token-from-session-0001")
        #expect(Array(tokens.dropFirst()).allSatisfy { $0 == "after-start" || $0 == "after-beat" })
        #expect(Set(tokens.compactMap { $0 }).count == tokens.count)  // never reused
    }

    @Test func releaseBeforeSendMeansNoStart() async {
        let harness = Self.harness()
        let model = await harness.online()
        model.pressBegan(.seekBackward)
        // Released in the same turn: the START has not had its turn yet.
        model.pressEnded(.seekBackward, completed: true)
        await model.waitUntilIdle()
        #expect(harness.transport.posts.isEmpty)
        #expect(model.lastResult == ResultLine(text: "REW CANCELLED BEFORE SEND · NOTHING SENT", tone: .nothingSent))
    }

    @Test func releaseDuringStartReleasesAsSoonAsStartAnswers() async {
        let harness = Self.harness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/cd/seek", Self.started("SEEK_STARTED", delayMs: 100))
        harness.transport.enqueue(path: "/api/v1/cd/seek", Self.released)
        model.pressBegan(.seekForward)
        try? await Task.sleep(for: .milliseconds(30))
        model.pressEnded(.seekForward, completed: true)
        await model.waitUntilIdle()
        #expect(Self.actions(harness.transport.posts(to: "/api/v1/cd/seek")) == ["START", "RELEASE"])
    }

    @Test func uncertainStartSendsOneStopOnlyRelease() async {
        let harness = Self.harness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/cd/seek", .json(502,
            #"{"result":"SERVICE_UNCERTAIN","physical_check_required":true,"csrf_token":"t2"}"#))
        harness.transport.enqueue(path: "/api/v1/cd/seek", Self.released)
        model.pressBegan(.seekForward)
        try? await Task.sleep(for: .milliseconds(50))
        model.pressEnded(.seekForward, completed: true)
        await model.waitUntilIdle()
        #expect(harness.transport.posts(to: "/api/v1/cd/seek") ==
                [#"{"action":"START","direction":"FORWARD"}"#, #"{"action":"RELEASE"}"#])
        #expect(model.lastResult == ResultLine(text: "FF START SERVICE UNCERTAIN · RELEASE TX VERIFIED · CHECK CHANGER",
                                               tone: .uncertain))
    }

    @Test func lostStartReplyAlsoReleases() async {
        let harness = Self.harness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/volume/hold", .failure(.timedOut))
        harness.transport.enqueue(path: "/api/v1/volume/hold", .json(200, #"{"result":"NOT_ACTIVE"}"#))
        model.pressBegan(.volumeDown)
        try? await Task.sleep(for: .milliseconds(250))
        await model.waitUntilIdle()
        #expect(Self.actions(harness.transport.posts(to: "/api/v1/volume/hold")) == ["START", "RELEASE"])
        #expect(model.lastResult?.tone == .uncertain)
    }

    @Test func refusedStartReleasesNothing() async {
        let harness = Self.harness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/volume/hold", .json(409, #"{"result":"VOLUME_BUSY","csrf_token":"t"}"#))
        model.pressBegan(.volumeUp)
        try? await Task.sleep(for: .milliseconds(250))
        await model.waitUntilIdle()
        #expect(Self.actions(harness.transport.posts(to: "/api/v1/volume/hold")) == ["START"])
        #expect(model.lastResult == ResultLine(text: "VOL+ VOLUME BUSY · NOTHING SENT", tone: .nothingSent))
    }

    @Test func serverEndedHoldIsReleasedAndReported() async {
        let harness = Self.harness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/cd/seek", Self.started("SEEK_STARTED"))
        harness.transport.enqueue(path: "/api/v1/cd/seek", .json(409, #"{"result":"NOT_ACTIVE","cd_last_result":"LEASE_EXPIRED"}"#))
        harness.transport.enqueue(path: "/api/v1/cd/seek", .json(200, #"{"result":"NOT_ACTIVE"}"#))
        model.pressBegan(.seekForward)
        try? await Task.sleep(for: .milliseconds(200))
        await model.waitUntilIdle()
        #expect(Self.actions(harness.transport.posts(to: "/api/v1/cd/seek")) == ["START", "HEARTBEAT", "RELEASE"])
        #expect(model.lastResult == ResultLine(text: "FF ENDED · LEASE EXPIRED", tone: .neutral))
        model.pressEnded(.seekForward, completed: true)   // the finger lifts later: nothing more
        await model.waitUntilIdle()
        #expect(harness.transport.posts(to: "/api/v1/cd/seek").count == 3)
    }

    @Test func failedHeartbeatStopsTheHold() async {
        let harness = Self.harness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/volume/hold", Self.started())
        harness.transport.enqueue(path: "/api/v1/volume/hold", .failure(.connectionLost))
        harness.transport.enqueue(path: "/api/v1/volume/hold", Self.released)
        model.pressBegan(.volumeUp)
        try? await Task.sleep(for: .milliseconds(400))
        await model.waitUntilIdle()
        #expect(Self.actions(harness.transport.posts(to: "/api/v1/volume/hold")) == ["START", "HEARTBEAT", "RELEASE"])
        #expect(model.activeHold == nil)
    }

    @Test func maximumHoldReleasesWithoutTheFinger() async {
        let harness = Self.harness()
        let model = await harness.online()
        harness.transport.setResponder(path: "/api/v1/cd/seek") { body in
            body.contains("START") ? Self.started("SEEK_STARTED", maxHoldMs: 150)
                : body.contains("HEARTBEAT") ? Self.renewed : Self.released
        }
        model.pressBegan(.seekBackward)
        try? await Task.sleep(for: .milliseconds(400))
        await model.waitUntilIdle()
        #expect(Self.actions(harness.transport.posts(to: "/api/v1/cd/seek")).last == "RELEASE")
        #expect(model.activeHold == nil)
        #expect(model.lastResult?.text == "REW RELEASE TX VERIFIED · SEEK RESULT UNVERIFIED")
    }

    @Test(arguments: ["APP INACTIVE", "NETWORK LOST", "VIEW CLOSED"])
    func interruptionsReleaseInsideABackgroundTask(reason: String) async {
        let harness = Self.harness()
        let model = await harness.online()
        Self.respondNormally(harness, path: "/api/v1/volume/hold")
        model.pressBegan(.volumeDown)
        try? await Task.sleep(for: .milliseconds(300))
        #expect(model.activeHold != nil)
        model.interrupt(reason: reason)
        await model.waitUntilIdle()
        try? await Task.sleep(for: .milliseconds(20))
        #expect(Self.actions(harness.transport.posts(to: "/api/v1/volume/hold")).last == "RELEASE")
        #expect(harness.background.began == ["Kenwood hold release"])
        #expect(harness.background.ended == 1)
        #expect(model.lastResult?.text == "VOL− RELEASE TX VERIFIED · VOLUME CHANGE UNVERIFIED")
    }

    @Test func interruptionDuringTheDecisionSendsNothing() async {
        let harness = Self.harness()
        let model = await harness.online()
        model.pressBegan(.volumeUp)
        model.interrupt(reason: "APP INACTIVE")
        try? await Task.sleep(for: .milliseconds(250))
        model.pressEnded(.volumeUp, completed: true)
        await model.waitUntilIdle()
        #expect(harness.transport.posts.isEmpty)
    }

    @Test func holdsNeverOverlap() async {
        let harness = Self.harness()
        let model = await harness.online()
        Self.respondNormally(harness, path: "/api/v1/volume/hold")
        model.pressBegan(.volumeUp)
        try? await Task.sleep(for: .milliseconds(250))
        model.pressBegan(.seekForward)
        model.pressBegan(.volumeDown)
        model.mute()
        try? await Task.sleep(for: .milliseconds(100))
        model.pressEnded(.volumeUp, completed: true)
        await model.waitUntilIdle()
        #expect(harness.transport.posts.allSatisfy { $0.url!.path == "/api/v1/volume/hold" })
        #expect(Self.actions(harness.transport.posts(to: "/api/v1/volume/hold")).filter { $0 == "START" }.count == 1)
    }

    @Test func withoutHoldCapabilityALongPressIsStillOneStep() async {
        let harness = ModelHarness(capabilities: Fixture.capabilities("volume_hold", ["enabled": false]))
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/volume/step", .json(200, #"{"result":"SERVICE_TX_OK"}"#))
        model.pressBegan(.volumeUp)
        try? await Task.sleep(for: .milliseconds(500))
        model.pressEnded(.volumeUp, completed: true)
        await model.waitUntilIdle()
        #expect(harness.transport.posts.map(\.url!.path) == ["/api/v1/volume/step"])
    }

    @Test func releaseUncertainIsReportedWithTheBackstop() async {
        let harness = Self.harness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/cd/seek", Self.started("SEEK_STARTED"))
        harness.transport.enqueue(path: "/api/v1/cd/seek", .failure(.timedOut))
        model.pressBegan(.seekForward)
        try? await Task.sleep(for: .milliseconds(20))
        model.pressEnded(.seekForward, completed: true)
        await model.waitUntilIdle()
        #expect(model.lastResult == ResultLine(
            text: "FF RELEASE NO REPLY (TIMEOUT) · LEASE ENDS SEEK WITHIN 2 s · CHECK CHANGER", tone: .uncertain))
    }

    @Test func releaseServiceUncertainIsAFault() async {
        let harness = Self.harness()
        let model = await harness.online()
        harness.transport.enqueue(path: "/api/v1/cd/seek", Self.started("SEEK_STARTED"))
        harness.transport.enqueue(path: "/api/v1/cd/seek", .json(502, #"{"result":"SERVICE_UNCERTAIN","physical_check_required":true}"#))
        harness.transport.enqueue(.status, .failure(.timedOut))   // keep the local latch visible
        model.pressBegan(.seekForward)
        try? await Task.sleep(for: .milliseconds(20))
        model.pressEnded(.seekForward, completed: true)
        await model.waitUntilIdle()
        try? await Task.sleep(for: .milliseconds(50))
        #expect(model.lastResult == ResultLine(text: "CD FAULT LATCHED · STOP AND INSPECT CHANGER", tone: .fault))
        #expect(model.localFault == .cd)
        #expect(!model.isEnabled(.cdSeek(forward: true)))
    }
}
