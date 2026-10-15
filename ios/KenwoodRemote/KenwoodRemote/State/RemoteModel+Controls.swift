import Foundation

/// Every control the screen can enable.
enum Control: Hashable, Sendable {
    case power, source(Source), tape2Monitor, mute, sync
    case volumeStep(up: Bool), volumeHold(up: Bool)
    case cdTrack(next: Bool), cdPlayback(PlaybackAction), cdSeek(forward: Bool), cdTrackSelect
}

/// The four press-and-hold keys.
enum HoldKey: Hashable, Sendable {
    case volumeUp, volumeDown, seekForward, seekBackward

    var isVolume: Bool { self == .volumeUp || self == .volumeDown }
    var direction: String {
        switch self {
        case .volumeUp: "UP"
        case .volumeDown: "DOWN"
        case .seekForward: "FORWARD"
        case .seekBackward: "BACKWARD"
        }
    }
    var label: String {
        switch self {
        case .volumeUp: "VOL+"
        case .volumeDown: "VOL−"
        case .seekForward: "FF"
        case .seekBackward: "REW"
        }
    }
    var holdControl: Control {
        switch self {
        case .volumeUp: .volumeHold(up: true)
        case .volumeDown: .volumeHold(up: false)
        case .seekForward: .cdSeek(forward: true)
        case .seekBackward: .cdSeek(forward: false)
        }
    }
    func command(_ action: HoldAction) -> ControlCommand {
        isVolume ? .volumeHold(action) : .cdSeek(action)
    }
    var startedResult: String { isVolume ? "HOLD_STARTED" : "SEEK_STARTED" }
    /// The Gateway's absolute maxima (volume 20 s, shared with CEC; seek 10 s).
    var maxHoldCapMs: Int { isVolume ? 20_000 : 10_000 }
    var check: String { isVolume ? "RECEIVER" : "CHANGER" }
    var effect: String { isVolume ? "VOLUME CHANGE" : "SEEK RESULT" }
}

/// The one hold in progress. Its guard is shared with GatewayClient, so a START
/// still waiting for its turn is never sent after the key was released.
@MainActor
final class ActiveHold {
    enum Phase: Equatable { case starting, holding, releasing }

    let key: HoldKey
    let sendGuard = SendGuard()
    let startedAt = ContinuousClock.now
    var holdId: String?
    var phase: Phase = .starting
    var releaseReason: String?

    init(key: HoldKey) { self.key = key }

    var isReleased: Bool { sendGuard.isCancelled }
}

/// Client-side spacing that mirrors the Gateway's (which stays authoritative):
/// a tap inside an interval is dropped, never queued.
struct ControlSpacing {
    var volumeCompleted: Date?
    var cdCompleted: Date?
    var sourceAttempted: Date?
    var powerCompleted: Date?
    var monitorCompleted: Date?
    var queryAttempted: Date?

    static let sourceMs = 1000
    static let powerMs = 2000
    static let monitorMs = 1000
    static let queryMs = 2000

    static func elapsed(_ since: Date?, _ ms: Int, now: Date) -> Bool {
        guard let since else { return true }
        return now.timeIntervalSince(since) * 1000 >= Double(ms)
    }
}

extension RemoteModel {
    // MARK: Enablement

    private func enabledOperation(_ name: OperationName) -> Capabilities.Operation? {
        guard let operation = capabilities?.operation(name), operation.enabled else { return nil }
        return operation
    }

    /// Enabled only when the Gateway advertises the operation (gate, session
    /// and eligible STATUS) and no fault has been reported since the last read.
    /// Busy state is not part of this: keys stay visually stable and taps
    /// during a running control are simply dropped.
    func isEnabled(_ control: Control) -> Bool {
        guard isOnline, let session = snapshot?.session else { return false }
        let serviceOK = localFault != .service
        let cdOK = localFault == nil
        switch control {
        case .power:
            return session.power && enabledOperation(.power) != nil && serviceOK
        case .source(let source):
            return session.source && serviceOK &&
                (enabledOperation(.source)?.availableSources ?? []).contains(source.rawValue)
        case .tape2Monitor:
            return session.tape2Monitor && enabledOperation(.tape2Monitor) != nil && serviceOK
        case .mute:
            return session.mute && enabledOperation(.mute) != nil && serviceOK
        case .sync:
            return session.receiverQuery && enabledOperation(.receiverQuery) != nil && serviceOK
        case .volumeStep(let up):
            return session.volumeStep && serviceOK &&
                (enabledOperation(.volumeStep)?.availableDirections ?? []).contains(up ? "UP" : "DOWN")
        case .volumeHold(let up):
            return session.volumeHold && serviceOK &&
                (enabledOperation(.volumeHold)?.availableDirections ?? []).contains(up ? "UP" : "DOWN")
        case .cdTrack(let next):
            return session.cdTrack && cdOK &&
                (enabledOperation(.cdTransport)?.availableActions ?? []).contains(next ? "NEXT" : "PREVIOUS")
        case .cdPlayback(let action):
            return session.cdPlayback && cdOK &&
                (enabledOperation(.cdPlayback)?.availableActions ?? []).contains(action.rawValue)
        case .cdSeek(let forward):
            return session.cdSeek && cdOK &&
                (enabledOperation(.cdSeek)?.availableDirections ?? []).contains(forward ? "FORWARD" : "BACKWARD")
        case .cdTrackSelect:
            return session.cdTrackSelect && enabledOperation(.cdTrackSelect) != nil && cdOK
        }
    }

    /// Volume keys accept a press when either a tap or a hold is possible.
    func isEnabled(_ key: HoldKey) -> Bool {
        switch key {
        case .volumeUp: isEnabled(.volumeStep(up: true)) || isEnabled(.volumeHold(up: true))
        case .volumeDown: isEnabled(.volumeStep(up: false)) || isEnabled(.volumeHold(up: false))
        case .seekForward, .seekBackward: isEnabled(key.holdControl)
        }
    }

    /// The Gateway's track range (at most 1–39 in v1).
    var trackRange: ClosedRange<Int> {
        let operation = capabilities?.operation(.cdTrackSelect)
        let low = max(1, operation?.minTrack ?? 1)
        let high = min(39, operation?.maxTrack ?? 39)
        return low...max(low, high)
    }

    /// A canonical track number in range, or nil (GO stays disabled).
    func validTrack(_ text: String) -> Int? {
        guard text.wholeMatch(of: /[1-9][0-9]?/) != nil, let track = Int(text),
              trackRange.contains(track) else { return nil }
        return track
    }

    /// The CD spacing from capabilities; 0 is the experimental no-cooldown
    /// setting, not a missing value. Anything invalid falls back to 1000 ms.
    var cdIntervalMs: Int {
        let value = capabilities?.operation(.cdTransport)?.minIntervalMs
            ?? capabilities?.operation(.cdPlayback)?.minIntervalMs
        guard let value, value == 0 || (200...1000).contains(value) else { return 1000 }
        return value
    }

    var volumeIntervalMs: Int {
        let value = capabilities?.operation(.volumeStep)?.minIntervalMs ?? 250
        return (0...5000).contains(value) ? value : 250
    }

    var holdDelayMs: Int {
        let value = capabilities?.operation(.volumeHold)?.holdDelayMs ?? 400
        return min(max(value, 150), 1000)
    }

    private var powerToggleAvailable: Bool {
        capabilities?.operation(.power)?.toggleAvailable == true
    }

    // MARK: Single-shot controls

    func tapPower() {
        guard isEnabled(.power), !busy, activeHold == nil else { return }
        guard powerToggleAvailable else {
            showingPowerChooser = true
            return
        }
        // A fresh observation the screen shows goes along as a guard only.
        let expected: String? = switch powerIndicator {
        case .on: "ON"
        case .standbyInferred: "OFF_INFERRED"
        case .unknown: nil
        }
        run(.powerToggle(expected: expected), control: .power, progress: "POWER",
            spaced: ControlSpacing.elapsed(spacing.powerCompleted, ControlSpacing.powerMs, now: now()),
            describe: { outcome, reply in
                if case .txVerified = outcome {
                    let was = reply?.decision?.state == "ON" ? "ON" : "SILENT TWICE"
                    return ResultLine(text: "POWER \(reply?.sent ?? "TOGGLE") TX VERIFIED · RECEIVER WAS \(was) · RECEIVER POWER UNVERIFIED",
                                      tone: .verified)
                }
                if case .nothingSent(let reason) = outcome, reply?.powerSent == false {
                    return ResultLine(text: "POWER TOGGLE \(Self.pretty(reason)) · NOTHING SENT · CHOOSE ON OR OFF",
                                      tone: .nothingSent)
                }
                return Self.line(label: "POWER \(reply?.sent ?? "TOGGLE")", effect: "RECEIVER POWER",
                                 check: "RECEIVER", outcome)
            },
            after: { outcome, reply in
                if case .nothingSent = outcome {
                    // Refused with nothing sent: offer the explicit choice, never a retry.
                    if reply?.powerSent == false { self.showingPowerChooser = true }
                } else {
                    self.spacing.powerCompleted = self.now()
                }
            })
    }

    func choosePower(on: Bool) {
        showingPowerChooser = false
        let label = "POWER \(on ? "ON" : "OFF")"
        run(.power(on: on), control: .power, progress: label,
            spaced: ControlSpacing.elapsed(spacing.powerCompleted, ControlSpacing.powerMs, now: now()),
            describe: { outcome, _ in
                Self.line(label: label, effect: "RECEIVER POWER", check: "RECEIVER", outcome)
            },
            after: { outcome, _ in
                if outcome.sentSomething { self.spacing.powerCompleted = self.now() }
            })
    }

    func selectSource(_ source: Source) {
        let spaced = ControlSpacing.elapsed(spacing.sourceAttempted, ControlSpacing.sourceMs, now: now())
        run(.source(source), control: .source(source), progress: source.rawValue, spaced: spaced,
            before: { self.spacing.sourceAttempted = self.now() },
            describe: { outcome, reply in
                if case .txVerified = outcome {
                    let stop = reply?.cdStop != nil ? " · CD STOP TX VERIFIED" : ""
                    return ResultLine(text: "\(source.rawValue) TX VERIFIED\(stop)"
                                      + Self.syncText(reply?.sync, kind: .source),
                                      tone: reply?.sync?.match == false ? .uncertain : .verified)
                }
                if reply?.stage == "CD_STOP" {
                    return ResultLine(text: "\(source.rawValue) TX VERIFIED · CD STOP \(Self.pretty(outcome.reason)) · CHECK CHANGER",
                                      tone: .uncertain)
                }
                return Self.line(label: source.rawValue, effect: "SELECTION", check: "RECEIVER", outcome)
            },
            after: { outcome, reply in
                if case .txVerified = outcome, let sync = reply?.sync {
                    self.sourceSince = sync.sinceMonoNs ?? self.sourceSince
                    self.spacing.queryAttempted = self.now()
                }
                // Leaving a fresh CD, the Gateway also sent CD_STOP (CD spacing).
                if reply?.cdStop != nil || reply?.stage == "CD_STOP" { self.spacing.cdCompleted = self.now() }
            })
    }

    func toggleMonitor() {
        run(.tape2Monitor, control: .tape2Monitor, progress: "TAPE2 MONITOR",
            spaced: ControlSpacing.elapsed(spacing.monitorCompleted, ControlSpacing.monitorMs, now: now()),
            describe: { outcome, reply in
                if case .txVerified = outcome {
                    return ResultLine(text: "TAPE2 MONITOR TX VERIFIED" + Self.syncText(reply?.sync, kind: .monitor),
                                      tone: reply?.sync?.match == false ? .uncertain : .verified)
                }
                return Self.line(label: "TAPE2 MONITOR", effect: "MONITOR CHANGE", check: "RECEIVER", outcome)
            },
            after: { outcome, reply in
                if outcome.sentSomething { self.spacing.monitorCompleted = self.now() }
                if case .txVerified = outcome, let sync = reply?.sync {
                    self.monitorSince = sync.sinceMonoNs ?? self.monitorSince
                    self.spacing.queryAttempted = self.now()
                }
            })
    }

    func mute() {
        run(.mute, control: .mute, progress: "MUTE", spaced: true,
            describe: { outcome, _ in
                Self.line(label: "MUTE", effect: "RECEIVER ACTION", check: "RECEIVER", outcome)
            })
    }

    /// One explicit receiver query (one ROOT 0x13 from the service). Never part
    /// of polling or pull-to-refresh.
    func sync() {
        run(.receiverQuery, control: .sync, progress: "SYNC",
            spaced: ControlSpacing.elapsed(spacing.queryAttempted, ControlSpacing.queryMs, now: now()),
            before: { self.spacing.queryAttempted = self.now() },
            describe: { outcome, reply in
                switch outcome {
                case .txVerified:
                    let source = reply?.observedSource ?? "?"
                    let monitor = reply?.observedMonitor ?? "?"
                    return ResultLine(text: "SYNC RECORDED · RECEIVER REPORTS \(source) · MONITOR \(monitor)",
                                      tone: .verified)
                case .uncertain(let reason):
                    return ResultLine(text: "SYNC \(Self.pretty(reply?.reason ?? reason)) · LAST OBSERVATION PRESERVED",
                                      tone: .uncertain)
                default:
                    return Self.line(label: "SYNC", effect: "OBSERVATION", check: "RECEIVER", outcome)
                }
            })
    }

    func volumeStep(up: Bool) {
        let label = up ? "VOL+" : "VOL−"
        run(.volumeStep(up: up), control: .volumeStep(up: up), progress: label,
            spaced: ControlSpacing.elapsed(spacing.volumeCompleted, volumeIntervalMs, now: now()),
            describe: { outcome, _ in
                Self.line(label: label, effect: "VOLUME CHANGE", check: "RECEIVER", outcome)
            },
            after: { outcome, _ in
                if outcome.sentSomething { self.spacing.volumeCompleted = self.now() }
            })
    }

    func cdTrack(next: Bool) {
        let label = next ? "NEXT" : "PREV"
        runCD(.cdTrack(next: next), control: .cdTrack(next: next), label: label, effect: "TRACK CHANGE")
    }

    func cdPlayback(_ action: PlaybackAction) {
        runCD(.cdPlayback(action), control: .cdPlayback(action), label: action.label, effect: "CHANGER ACTION")
    }

    func selectTrack(_ track: Int) {
        guard trackRange.contains(track) else { return }
        runCD(.cdTrackSelect(track), control: .cdTrackSelect, label: "TRACK \(track)", effect: "TRACK SELECTION")
    }

    private func runCD(_ command: ControlCommand, control: Control, label: String, effect: String) {
        run(command, control: control, progress: label,
            spaced: ControlSpacing.elapsed(spacing.cdCompleted, cdIntervalMs, now: now()),
            describe: { outcome, _ in
                Self.line(label: label, effect: effect, check: "CHANGER", outcome)
            },
            after: { outcome, _ in
                if outcome.sentSomething { self.spacing.cdCompleted = self.now() }
            })
    }

    /// Starts one control unless one is already running or not allowed: the tap
    /// is then dropped. `busy` is set before this returns, so a second tap in
    /// the same run-loop turn can never start a duplicate.
    private func run(_ command: ControlCommand, control: Control, progress: String, spaced: Bool,
                     before: () -> Void = {},
                     describe: @escaping @MainActor (ControlOutcome, ControlReply?) -> ResultLine,
                     after: @escaping @MainActor (ControlOutcome, ControlReply?) -> Void = { _, _ in }) {
        guard !busy, activeHold == nil, pendingPress == nil, let client,
              isEnabled(control), spaced else { return }
        busy = true
        before()
        lastResult = ResultLine(text: "\(progress) · SENDING", tone: .progress)
        // If the app leaves the foreground now, let the reply still arrive.
        let endBackgroundTask = beginBackgroundTask("Kenwood control")
        controlTask = Task {
            defer { endBackgroundTask() }
            await self.cancelRefresh()
            let exchange = await client.send(command)
            let outcome = ControlOutcome(exchange)
            after(outcome, exchange.reply)
            self.noteFault(outcome)
            self.lastResult = describe(outcome, exchange.reply)
            self.busy = false
            await self.refreshNow()
        }
    }

    private func noteFault(_ outcome: ControlOutcome) {
        if case .fault(let result) = outcome {
            localFault = result == "CD_FAULT_LATCHED" ? .cd : .service
        }
    }

    // MARK: Press-and-hold

    /// A key went down. Volume: decide tap or hold after `hold_delay_ms`.
    /// Seek has no tap action and starts at once.
    func pressBegan(_ key: HoldKey) {
        guard pendingPress == nil, activeHold == nil, !busy, isEnabled(key) else { return }
        guard key.isVolume else {
            beginHold(key)
            return
        }
        pendingPress = key
        guard isEnabled(key.holdControl) else { return }  // Tap only.
        let delay = holdDelayMs
        pressTimer = Task {
            try? await Task.sleep(for: .milliseconds(delay))
            guard !Task.isCancelled else { return }
            self.holdDelayElapsed(key)
        }
    }

    /// The key went up (`completed`) or its touch was cancelled. A completed
    /// short press is one tap; a cancelled one sends nothing.
    func pressEnded(_ key: HoldKey, completed: Bool) {
        if pendingPress == key {
            cancelPendingPress()
            if completed, key.isVolume { volumeStep(up: key == .volumeUp) }
            return
        }
        if activeHold?.key == key { releaseHold(reason: completed ? "RELEASED" : "TOUCH CANCELLED") }
    }

    private func holdDelayElapsed(_ key: HoldKey) {
        guard pendingPress == key else { return }
        pendingPress = nil
        pressTimer = nil
        // The gesture is now a hold; it never also becomes a step.
        beginHold(key)
    }

    private func cancelPendingPress() {
        pressTimer?.cancel()
        pressTimer = nil
        pendingPress = nil
    }

    private func holdSpaced(_ key: HoldKey) -> Bool {
        key.isVolume
            ? ControlSpacing.elapsed(spacing.volumeCompleted, volumeIntervalMs, now: now())
            : ControlSpacing.elapsed(spacing.cdCompleted, cdIntervalMs, now: now())
    }

    private func beginHold(_ key: HoldKey) {
        guard activeHold == nil, !busy, let client, isEnabled(key.holdControl), holdSpaced(key) else { return }
        let hold = ActiveHold(key: key)
        activeHold = hold
        busy = true
        lastResult = ResultLine(text: "\(key.label) STARTING", tone: .progress)
        holdTask = Task { await self.runHold(hold, client: client) }
    }

    /// Ends the hold: idempotent, and honoured even while START is in flight.
    func releaseHold(reason: String) {
        guard let hold = activeHold, !hold.isReleased else { return }
        hold.releaseReason = reason
        hold.sendGuard.cancel()
    }

    private func runHold(_ hold: ActiveHold, client: GatewayClient) async {
        let key = hold.key
        await cancelRefresh()
        let start = await client.send(key.command(.start(direction: key.direction)), guard: hold.sendGuard)
        if case .notAttempted(let reason) = start {
            finishHold(hold, ResultLine(text: "\(key.label) \(Self.pretty(reason)) · NOTHING SENT", tone: .nothingSent),
                       sent: false)
            return
        }
        guard case .response(200, let reply?, true) = start, reply.result == key.startedResult,
              let holdId = reply.holdId, ControlCommand.isValidHoldId(holdId) else {
            await abandonStart(hold, start, client: client)
            return
        }
        hold.holdId = holdId
        hold.phase = .holding
        if !hold.isReleased {
            lastResult = ResultLine(text: key.isVolume ? "\(key.label) HOLDING · VOLUME CHANGE UNVERIFIED"
                                                      : "\(key.label) SEEKING · CHANGER ACTION UNVERIFIED",
                                    tone: .progress)
        }
        let heartbeatMs = min(max(reply.heartbeatMs ?? 500, 50), 1000)
        let advertised = capabilities?.operation(key.isVolume ? .volumeHold : .cdSeek)?.maxHoldMs
        let maxHoldMs = [reply.maxHoldMs, advertised, key.maxHoldCapMs].compactMap { $0 }.min()!
        var ended: String?
        renewals: while !hold.isReleased {
            await pause(ms: heartbeatMs, unlessReleased: hold)
            if hold.isReleased { break }
            if ContinuousClock.now - hold.startedAt >= .milliseconds(maxHoldMs) {
                hold.releaseReason = "MAX_HOLD"
                break
            }
            let beat = await client.send(key.command(.heartbeat(holdId: holdId)))
            switch beat {
            case .response(200, let reply?, _) where reply.result == "LEASE_RENEWED":
                continue
            case .response(_, let reply?, _) where reply.result == "NOT_ACTIVE":
                ended = reply.cdLastResult ?? reply.reason ?? "NOT ACTIVE"
                break renewals
            default:
                // Anything else (lost reply, refusal, network): stop now. The
                // RELEASE below is stop-only; the service lease is the backstop.
                hold.releaseReason = "HEARTBEAT \(Self.pretty(ControlOutcome(beat).reason))"
                break renewals
            }
        }
        // Exactly one RELEASE, also after an expiry: it is stop-only and the
        // Gateway answers NOT_ACTIVE without transmitting when nothing is held.
        hold.phase = .releasing
        let release = await client.send(key.command(.release(holdId: holdId)))
        finishHold(hold, Self.releaseLine(key, release, ended: ended), sent: true)
    }

    /// START was refused, uncertain or unreadable.
    private func abandonStart(_ hold: ActiveHold, _ start: ControlExchange, client: GatewayClient) async {
        let key = hold.key
        let outcome = ControlOutcome(start)
        switch outcome {
        case .nothingSent, .fault:
            // A refusal before transmission: nothing to release.
            finishHold(hold, Self.line(label: key.label, effect: key.effect, check: key.check, outcome),
                       sent: false)
        case .uncertain, .txVerified:
            // A hold may exist that this app cannot name: release this session's
            // hold once (stop-only, never another START).
            hold.phase = .releasing
            let release = await client.send(key.command(.release(holdId: nil)))
            let released = release.reply?.result == "RELEASED"
            finishHold(hold, ResultLine(
                text: "\(key.label) START \(Self.pretty(outcome.reason)) · "
                    + (released ? "RELEASE TX VERIFIED · CHECK \(key.check)" : "CHECK \(key.check)"),
                tone: .uncertain), sent: true)
        }
    }

    private func finishHold(_ hold: ActiveHold, _ line: ResultLine, sent: Bool) {
        guard activeHold === hold else { return }
        activeHold = nil
        busy = false
        if sent {
            if hold.key.isVolume { spacing.volumeCompleted = now() } else { spacing.cdCompleted = now() }
        }
        if line.tone == .fault { localFault = hold.key.isVolume ? .service : .cd }
        lastResult = line
        Task { await self.refreshNow() }
    }

    private func pause(ms: Int, unlessReleased hold: ActiveHold) async {
        let deadline = ContinuousClock.now + .milliseconds(ms)
        while !hold.isReleased, ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(10))
        }
    }

    // MARK: Interruptions

    /// The app left the foreground, a held view disappeared or the network
    /// went away: no press may outlive it. The release request runs inside an
    /// OS background task so it can still be sent.
    func interrupt(reason: String) {
        cancelPendingPress()
        guard let hold = activeHold, let holdTask else { return }
        if !hold.isReleased { releaseHold(reason: reason) }
        let end = beginBackgroundTask("Kenwood hold release")
        Task {
            await holdTask.value
            end()
        }
    }

    func stopInteraction(reason: String) {
        interrupt(reason: reason)
        showingPowerChooser = false
    }

    /// Waits for every running control and hold (tests).
    func waitUntilIdle() async {
        while true {
            let tasks = [controlTask, holdTask, pressTimer].compactMap { $0 }
            for task in tasks { await task.value }
            await Task.yield()
            if !busy && activeHold == nil && pendingPress == nil { return }
            try? await Task.sleep(for: .milliseconds(5))
        }
    }

    // MARK: Wording

    static func pretty(_ result: String) -> String { result.replacingOccurrences(of: "_", with: " ") }

    static func line(label: String, effect: String, check: String, _ outcome: ControlOutcome) -> ResultLine {
        switch outcome {
        case .txVerified:
            ResultLine(text: "\(label) TX VERIFIED · \(effect) UNVERIFIED", tone: .verified)
        case .nothingSent(let reason):
            ResultLine(text: "\(label) \(pretty(reason)) · NOTHING SENT", tone: .nothingSent)
        case .uncertain(let reason):
            ResultLine(text: "\(label) \(pretty(reason)) · UNCERTAIN · CHECK \(check)", tone: .uncertain)
        case .fault(let result):
            ResultLine(text: result == "CD_FAULT_LATCHED"
                       ? "CD FAULT LATCHED · STOP AND INSPECT CHANGER"
                       : "\(label) FAULT LATCHED · CHECK RECEIVER · RESTART REQUIRED", tone: .fault)
        }
    }

    enum SyncKind { case source, monitor }

    /// The Gateway's final SYNC after a verified source or MONITOR control. Only
    /// a later observation changes the indicators; this is the reply's report.
    static func syncText(_ sync: SyncReply?, kind: SyncKind) -> String {
        let unverified = kind == .source ? "SELECTION UNVERIFIED" : "MONITOR CHANGE UNVERIFIED"
        guard let sync, let result = sync.result, result != "NOT_ENABLED" else { return " · \(unverified)" }
        guard result == "OBSERVATION_RECORDED" else {
            return " · SYNC \(pretty(sync.reason ?? result)) · \(unverified)"
        }
        let value = kind == .source ? sync.observedSource : sync.observedMonitor.map { "MONITOR \($0)" }
        guard let value else { return " · SYNC RECORDED" }
        return " · RECEIVER REPORTS \(value)" + (sync.match == false ? " · CHECK RECEIVER" : "")
    }

    static func releaseLine(_ key: HoldKey, _ release: ControlExchange, ended: String?) -> ResultLine {
        switch release {
        case .response(200, let reply?, _) where reply.result == "RELEASED":
            let note = ended.map { " · \(pretty($0))" } ?? ""
            return ResultLine(text: "\(key.label) RELEASE TX VERIFIED\(note) · \(key.effect) UNVERIFIED", tone: .verified)
        case .response(_, let reply?, _) where reply.result == "NOT_ACTIVE":
            return ResultLine(text: "\(key.label) ENDED · \(pretty(ended ?? reply.cdLastResult ?? "NOT ACTIVE"))",
                              tone: .neutral)
        default:
            let outcome = ControlOutcome(release)
            if case .uncertain(let reason) = outcome, reason == "SERVICE_UNCERTAIN" {
                return ResultLine(text: key.isVolume ? "VOLUME FAULT LATCHED · CHECK RECEIVER · RESTART REQUIRED"
                                                     : "CD FAULT LATCHED · STOP AND INSPECT CHANGER",
                                  tone: .fault)
            }
            if case .fault = outcome { return line(label: "\(key.label) RELEASE", effect: key.effect, check: key.check, outcome) }
            let backstop = key.isVolume ? "SERVICE STOPS VOLUME" : "LEASE ENDS SEEK"
            return ResultLine(text: "\(key.label) RELEASE \(pretty(outcome.reason)) · \(backstop) WITHIN 2 s · CHECK \(key.check)",
                              tone: .uncertain)
        }
    }
}

extension ControlOutcome {
    /// Anything but a clean refusal may have started Gateway-side spacing.
    var sentSomething: Bool {
        if case .nothingSent = self { return false }
        return true
    }
}
