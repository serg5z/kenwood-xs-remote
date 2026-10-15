import SwiftUI
import Testing
import UIKit
@testable import KenwoodRemote

/// The views above the keys must keep one height through every state a command
/// passes through, or SOURCE, MONITOR, VOLUME and CD move under the finger.
@MainActor @Suite struct LayoutStabilityTests {
    /// iPhone 13 mini and a 6.1" phone, minus the 16 pt gutters.
    nonisolated static let widths: [CGFloat] = [343, 361]

    static let lines: [ResultLine?] = [
        nil,
        ResultLine(text: "MUTE · SENDING", tone: .progress),
        ResultLine(text: "VOL+ STARTING", tone: .progress),
        ResultLine(text: "MUTE TX VERIFIED · RECEIVER ACTION UNVERIFIED", tone: .verified),
        ResultLine(text: "FF ENDED · LEASE EXPIRED", tone: .neutral),
        ResultLine(text: "POWER TOGGLE POWER STATE UNKNOWN · NOTHING SENT · CHOOSE ON OR OFF", tone: .nothingSent),
        ResultLine(text: "VOL− RELEASE NO REPLY (TIMEOUT) · SERVICE STOPS VOLUME WITHIN 2 s · CHECK RECEIVER",
                   tone: .uncertain),
        ResultLine(text: "TUNER TX VERIFIED · CD STOP TX VERIFIED · SYNC OBSERVATION FAILED · SELECTION UNVERIFIED",
                   tone: .uncertain),
        ResultLine(text: "CD FAULT LATCHED · STOP AND INSPECT CHANGER", tone: .fault),
    ]

    func height<V: View>(_ view: V, width: CGFloat) -> CGFloat {
        UIHostingController(rootView: view)
            .sizeThatFits(in: CGSize(width: width, height: CGFloat.greatestFiniteMagnitude)).height
    }

    @Test(arguments: widths)
    func resultLineKeepsItsHeight(width: CGFloat) async {
        let model = await ModelHarness().online()
        var heights: Set<CGFloat> = []
        for line in Self.lines {
            model.lastResult = line
            heights.insert(height(ResultBanner(model: model), width: width))
        }
        let faulted = await ModelHarness(status: Fixture.status(["cd_state": "FAULT_LATCHED"])).online()
        heights.insert(height(ResultBanner(model: faulted), width: width))
        let offline = ModelHarness().model
        heights.insert(height(ResultBanner(model: offline), width: width))
        #expect(heights.count == 1, "result line heights \(heights.sorted())")
    }

    @Test(arguments: widths)
    func sourceSectionKeepsItsHeight(width: CGFloat) async {
        let harness = ModelHarness()
        let model = await harness.online()
        var heights: Set<CGFloat> = [height(SourceSelector(model: model), width: width)]
        model.sourceSince = UInt64.max          // awaiting the receiver's report
        heights.insert(height(SourceSelector(model: model), width: width))
        let lastKnown = await ModelHarness(status: Self.statusWithSource(freshness: "LAST_KNOWN")).online()
        heights.insert(height(SourceSelector(model: lastKnown), width: width))
        harness.transport.enqueue(.status, .failure(.timedOut))
        await model.refresh()                   // connection lost: the longest summary
        heights.insert(height(SourceSelector(model: model), width: width))
        #expect(heights.count == 1, "source section heights \(heights.sorted())")
    }

    @Test(arguments: widths)
    func headerKeepsItsSize(width: CGFloat) async {
        let harness = ModelHarness()
        let model = await harness.online()
        #expect(model.powerIndicator == .on)
        let online = UIHostingController(rootView: HeaderView(model: model))
            .sizeThatFits(in: CGSize(width: width, height: CGFloat.greatestFiniteMagnitude))
        harness.transport.enqueue(.status, .failure(.timedOut))
        await model.refresh()                   // UNKNOWN power, "Connection problem"
        let offline = UIHostingController(rootView: HeaderView(model: model))
            .sizeThatFits(in: CGSize(width: width, height: CGFloat.greatestFiniteMagnitude))
        #expect(online == offline)
    }

    /// The connected fixture with the source observation's freshness changed.
    static func statusWithSource(freshness: String) -> Data {
        var root = try! JSONSerialization.jsonObject(with: Fixture.data("status-connected")) as! [String: Any]
        var receiver = root["receiver"] as! [String: Any]
        var source = receiver["source"] as! [String: Any]
        source["freshness"] = freshness
        source["age_ms"] = 95_000
        receiver["source"] = source
        root["receiver"] = receiver
        return try! JSONSerialization.data(withJSONObject: root)
    }
}
