import Foundation
import Testing
@testable import KenwoodRemote

@Suite struct ReceiverDisplayTests {
    static func observation(_ value: String, _ freshness: String, age: Int? = 1000) -> ReceiverObservation {
        ReceiverObservation(value: value, observedMonoNs: value == "UNKNOWN" ? 0 : 1, evidence: "QUERY",
                    ageMs: age, freshness: freshness)
    }

    static func receiver(source: String = "CD", sourceFreshness: String = "FRESH",
                         monitor: String = "OFF", monitorFreshness: String = "FRESH",
                         integrity: String = "HEALTHY") -> ReceiverObservations {
        ReceiverObservations(source: observation(source, sourceFreshness),
                             monitor: observation(monitor, monitorFreshness),
                             integrity: integrity)
    }

    @Test func freshSourceIsHighlightedAsObserved() {
        let indicator = SourceIndicator(receiver: Self.receiver(), connected: true)
        #expect(indicator.highlight(for: .cd) == .fresh)
        #expect(Source.allCases.filter { $0 != .cd }.allSatisfy { indicator.highlight(for: $0) == .none })
        #expect(indicator.displayedSource == .cd)
    }

    @Test func lastKnownSourceIsDimmedNotFresh() {
        let indicator = SourceIndicator(receiver: Self.receiver(source: "TUNER", sourceFreshness: "LAST_KNOWN"),
                                        connected: true)
        #expect(indicator.highlight(for: .tuner) == .lastKnown)
        #expect(indicator.quality == .lastKnown)
        #expect(indicator.summary.hasPrefix("Last known TUNER"))
    }

    @Test func uncertainIntegrityHighlightsNothing() {
        let indicator = SourceIndicator(receiver: Self.receiver(integrity: "UNCERTAIN"), connected: true)
        #expect(Source.allCases.allSatisfy { indicator.highlight(for: $0) == .none })
        #expect(indicator.quality == .uncertain)
        #expect(indicator.displayedSource == nil)
    }

    @Test func unknownSourceHighlightsNothing() {
        let indicator = SourceIndicator(receiver: Self.receiver(source: "UNKNOWN", sourceFreshness: "UNKNOWN"),
                                        connected: true)
        #expect(indicator.quality == .unknown)
        #expect(indicator.displayedSource == nil)
    }

    @Test func lostConnectionHighlightsNothingEvenWithAFreshLastReply() {
        let indicator = SourceIndicator(receiver: Self.receiver(), connected: false)
        #expect(indicator.quality == .connectionLost)
        #expect(indicator.highlight(for: .cd) == .none)
    }

    @Test func tape2ObservationIsShownButMatchesNoSourceKey() {
        let indicator = SourceIndicator(receiver: Self.receiver(source: "TAPE2"), connected: true)
        #expect(indicator.observed == "TAPE2")
        #expect(indicator.displayedSource == nil)
        #expect(Source.allCases.allSatisfy { indicator.highlight(for: $0) == .none })
    }

    @Test func monitorShowsOnlyFreshHealthyValues() {
        #expect(MonitorIndicator(receiver: Self.receiver(monitor: "ON"), connected: true) == .on)
        #expect(MonitorIndicator(receiver: Self.receiver(monitor: "OFF"), connected: true) == .off)
        #expect(MonitorIndicator(receiver: Self.receiver(monitor: "ON", monitorFreshness: "LAST_KNOWN"),
                                 connected: true) == .unknown)
        #expect(MonitorIndicator(receiver: Self.receiver(monitor: "ON", integrity: "UNCERTAIN"),
                                 connected: true) == .unknown)
        #expect(MonitorIndicator(receiver: Self.receiver(monitor: "ON"), connected: false) == .unknown)
    }

    @Test func powerShowsOnlyFreshObservations() {
        func power(_ value: String, _ freshness: String) -> PowerObservation {
            PowerObservation(value: value, observedMonoNs: 1, ageMs: 10, evidence: "QUERY", freshness: freshness)
        }
        #expect(PowerIndicator(power: power("ON", "FRESH"), connected: true) == .on)
        #expect(PowerIndicator(power: power("OFF_INFERRED", "FRESH"), connected: true) == .standbyInferred)
        #expect(PowerIndicator(power: power("ON", "STALE"), connected: true) == .unknown)
        #expect(PowerIndicator(power: power("UNKNOWN", "UNKNOWN"), connected: true) == .unknown)
        #expect(PowerIndicator(power: power("ON", "FRESH"), connected: false) == .unknown)
        #expect(PowerIndicator(power: nil, connected: true) == .unknown)
    }
}

@Suite struct ContextPanelStateTests {
    @Test func cdObservationExpandsTheCDPanel() {
        var panel = ContextPanelState()
        panel.observe(.cd)
        #expect(panel.visiblePanel == .cd && panel.isExpanded)
    }

    @Test func manualCollapseSticksWhileTheSourceIsUnchanged() {
        var panel = ContextPanelState()
        panel.observe(.cd)
        panel.toggle()
        panel.observe(.cd)
        #expect(!panel.isExpanded)
    }

    @Test func returningToCDExpandsAgain() {
        var panel = ContextPanelState()
        panel.observe(.cd)
        panel.toggle()
        panel.observe(.video1)
        #expect(panel.visiblePanel == nil)
        panel.observe(.cd)
        #expect(panel.isExpanded)
    }

    @Test func unknownObservationKeepsTheCurrentPanel() {
        var panel = ContextPanelState()
        panel.observe(.cd)
        panel.observe(nil)
        #expect(panel.visiblePanel == .cd && panel.isExpanded)
    }

    @Test func sourcesWithoutPanelsShowNone() {
        for source in [Source.tuner, .phono, .video2, .tape1] {
            var panel = ContextPanelState()
            panel.observe(source)
            panel.toggle()
            #expect(panel.visiblePanel == nil && !panel.isExpanded)
        }
    }
}
