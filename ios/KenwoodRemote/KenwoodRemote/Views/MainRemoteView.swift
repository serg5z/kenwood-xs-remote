import SwiftUI

/// Main screen, top to bottom: header and POWER, result line, sources, volume,
/// then the contextual panel for the observed source.
struct MainRemoteView: View {
    @Bindable var model: RemoteModel
    var networkPath: NetworkPathWatcher?
    @Environment(\.scenePhase) private var scenePhase
    @State private var showingCredentials = false
    @State private var showingDetails = false

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 20) {
                    HeaderView(model: model)
                    ResultBanner(model: model)
                    SourceSelector(model: model)
                    VolumeControls(model: model)
                    ContextualPanel(model: model)
                }
                .padding(.horizontal, 16)
                .padding(.bottom, 24)
            }
            .scrollDismissesKeyboard(.interactively)
            // Pull-to-refresh only reads: it never sends SYNC or any control.
            .refreshable { await model.refresh() }
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button("Details", systemImage: "info.circle") { showingDetails = true }
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Account", systemImage: "person.crop.circle") { showingCredentials = true }
                }
            }
            .navigationBarTitleDisplayMode(.inline)
        }
        .task(id: scenePhase) {
            // Poll only in the foreground; leaving it cancels this task.
            if scenePhase == .active { await model.pollWhileActive() }
        }
        .onChange(of: scenePhase) { _, phase in
            // Inactive covers Control Center, calls and the app switcher.
            if phase != .active { model.interrupt(reason: "APP INACTIVE") }
        }
        .task {
            guard let networkPath else { return }
            for await usable in networkPath.updates() where !usable {
                model.interrupt(reason: "NETWORK LOST")
            }
        }
        .sheet(isPresented: $showingCredentials) { CredentialsView(model: model) }
        .sheet(isPresented: $showingDetails) { DiagnosticsView(model: model) }
        .onAppear { if model.connection == .needsCredentials { showingCredentials = true } }
        .onChange(of: model.connection) { _, state in
            if state == .needsCredentials || state == .authenticationRejected {
                showingCredentials = true
            }
        }
    }
}

/// The result of the last control, or a fault the service reports. Results
/// describe transmissions; indicators elsewhere describe observations.
struct ResultBanner: View {
    let model: RemoteModel

    /// Lines reserved whatever the message: the controls below must not move
    /// while a command goes from idle to SENDING to its result.
    static let reservedLines = 2

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            symbol
                .frame(width: 18, height: 18)
            // The hidden full-size placeholder alone sets the height; the
            // message (which may shrink to fit) is laid over it.
            Text(String(repeating: "\n", count: Self.reservedLines - 1))
                .hidden()
                .frame(maxWidth: .infinity, alignment: .leading)
                .overlay(alignment: .topLeading) {
                    Text(message)
                        .lineLimit(Self.reservedLines)
                        .minimumScaleFactor(0.75)
                }
        }
        .font(.footnote.weight(.medium))
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(10)
        .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 10))
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.updatesFrequently)
    }

    private var message: String {
        if let fault = model.faultBanner { return fault }
        if let line = model.lastResult { return line.text }
        return model.isOnline ? "Ready · results show transmission, not receiver action"
                              : model.connection.title
    }

    @ViewBuilder private var symbol: some View {
        if model.faultBanner != nil {
            Image(systemName: "exclamationmark.octagon.fill").foregroundStyle(.red)
        } else if let line = model.lastResult {
            if line.tone == .progress {
                ProgressView().controlSize(.mini)
            } else {
                Image(systemName: icon(line.tone)).foregroundStyle(color(line.tone))
            }
        } else {
            Image(systemName: "dot.radiowaves.left.and.right").foregroundStyle(.secondary)
        }
    }

    private func icon(_ tone: ResultLine.Tone) -> String {
        switch tone {
        case .verified: "checkmark.circle"
        case .neutral, .progress: "info.circle"
        case .nothingSent: "minus.circle"
        case .uncertain: "questionmark.diamond.fill"
        case .fault: "exclamationmark.octagon.fill"
        }
    }

    private func color(_ tone: ResultLine.Tone) -> Color {
        switch tone {
        case .verified: .green
        case .neutral, .progress, .nothingSent: .secondary
        case .uncertain: .orange
        case .fault: .red
        }
    }
}

// MARK: Header

struct HeaderView: View {
    @Bindable var model: RemoteModel

    var body: some View {
        HStack(alignment: .center, spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Text("KENWOOD").font(.title2.weight(.heavy)).tracking(2)
                Text("KR-V6070").font(.caption.weight(.medium)).foregroundStyle(.secondary)
                ConnectionBadge(state: model.connection)
            }
            Spacer()
            VStack(spacing: 4) {
                PowerButton(indicator: model.powerIndicator, enabled: model.isEnabled(.power)) {
                    model.tapPower()
                }
                // Fixed to one full-size line at the button's width, so POWER
                // never moves whatever the label.
                Text(verbatim: " ")
                    .font(.caption2)
                    .frame(width: 56)
                    .overlay {
                        Text(model.powerIndicator.shortLabel)
                            .font(.caption2).foregroundStyle(.secondary)
                            .lineLimit(1)
                            .minimumScaleFactor(0.6)
                    }
            }
        }
        .padding(.top, 8)
        .confirmationDialog("Receiver power", isPresented: $model.showingPowerChooser,
                            titleVisibility: .visible) {
            Button("Power ON") { model.choosePower(on: true) }
            Button("Power OFF", role: .destructive) { model.choosePower(on: false) }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Sends exactly the chosen command. The receiver's power is not read back.")
        }
    }
}

struct ConnectionBadge: View {
    let state: ConnectionState

    var body: some View {
        HStack(spacing: 6) {
            Circle().fill(color).frame(width: 8, height: 8)
            Text(state.title).font(.caption.weight(.semibold)).lineLimit(1)
        }
        .padding(.top, 4)
        .accessibilityElement(children: .combine)
    }

    private var color: Color {
        switch state {
        case .online: .green
        case .connecting: .yellow
        case .serviceUnavailable, .needsCredentials: .orange
        default: .red
        }
    }
}

struct PowerButton: View {
    let indicator: PowerIndicator
    let enabled: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: "power")
                .font(.title2.weight(.bold))
                .foregroundStyle(.white)
                .frame(width: 56, height: 56)
                .background(Circle().fill(Color.red.opacity(enabled ? 0.9 : 0.4)))
                .overlay { ring }
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .accessibilityLabel("Power. \(indicator.label)")
    }

    // Green ring: fresh observed ON. Dashed: fresh inferred standby. None: unknown.
    @ViewBuilder private var ring: some View {
        switch indicator {
        case .on: Circle().strokeBorder(.green, lineWidth: 3).padding(-5)
        case .standbyInferred:
            Circle().strokeBorder(.secondary, style: StrokeStyle(lineWidth: 2, dash: [4, 3])).padding(-5)
        case .unknown: EmptyView()
        }
    }
}

extension PowerIndicator {
    var shortLabel: String {
        switch self {
        case .on: "ON"
        case .standbyInferred: "STANDBY?"
        case .unknown: "UNKNOWN"
        }
    }
}

// MARK: Sources

struct SourceSelector: View {
    let model: RemoteModel

    private let columns = Array(repeating: GridItem(.flexible(), spacing: 10), count: 3)

    var body: some View {
        let indicator = model.sourceIndicator
        VStack(alignment: .leading, spacing: 10) {
            SectionTitle("Source")
            LazyVGrid(columns: columns, spacing: 10) {
                ForEach(Source.allCases) { source in
                    Button(source.rawValue) { model.selectSource(source) }
                        .buttonStyle(RemoteKeyStyle(fill: fill(indicator, source)))
                        .disabled(!model.isEnabled(.source(source)))
                        .accessibilityValue(accessibility(indicator, source))
                }
            }
            HStack {
                // One line in every state: a wrapped summary moves every key below.
                Text(indicator.summary)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
                Spacer(minLength: 8)
                // One explicit receiver query; never automatic.
                Button("SYNC", systemImage: "arrow.triangle.2.circlepath") { model.sync() }
                    .font(.caption.weight(.semibold))
                    .buttonStyle(.bordered)
                    .controlSize(.small)
                    .disabled(!model.isEnabled(.sync))
            }
            Button(monitorLabel) { model.toggleMonitor() }
                .buttonStyle(RemoteKeyStyle(fill: model.monitorIndicator == .on ? .observed : .plain, height: 44))
                .disabled(!model.isEnabled(.tape2Monitor))
        }
    }

    private func fill(_ indicator: SourceIndicator, _ source: Source) -> KeyFace.Fill {
        switch indicator.highlight(for: source) {
        case .fresh: .observed
        case .lastKnown: .lastKnown
        case .none: .plain
        }
    }

    private func accessibility(_ indicator: SourceIndicator, _ source: Source) -> String {
        switch indicator.highlight(for: source) {
        case .fresh: "Observed selected"
        case .lastKnown: "Last known selection"
        case .none: ""
        }
    }

    private var monitorLabel: String {
        switch model.monitorIndicator {
        case .on: "TAPE2 MONITOR ON"
        case .off: "TAPE2 MONITOR OFF"
        case .unknown: "TAPE2 MONITOR"
        }
    }
}

// MARK: Volume

struct VolumeControls: View {
    let model: RemoteModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            SectionTitle("Volume")
            HStack(spacing: 10) {
                HoldableKey(key: .volumeDown, model: model, height: 64) {
                    Image(systemName: "minus").font(.title2.weight(.bold))
                }
                .accessibilityLabel("Volume down")
                .accessibilityAction { model.volumeStep(up: false) }
                Button("MUTE") { model.mute() }
                    .buttonStyle(RemoteKeyStyle(height: 64))
                    .disabled(!model.isEnabled(.mute))
                HoldableKey(key: .volumeUp, model: model, height: 64) {
                    Image(systemName: "plus").font(.title2.weight(.bold))
                }
                .accessibilityLabel("Volume up")
                .accessibilityAction { model.volumeStep(up: true) }
            }
        }
    }
}

// MARK: Contextual panel

struct ContextualPanel: View {
    @Bindable var model: RemoteModel

    var body: some View {
        if let source = model.panel.visiblePanel {
            // Collapsing would remove a held FF/REW key: not during a hold.
            let holding = model.activeHold != nil
            VStack(alignment: .leading, spacing: 12) {
                Button {
                    withAnimation(.snappy) { model.panel.toggle() }
                } label: {
                    HStack {
                        SectionTitle("\(source.rawValue) controls")
                        Spacer()
                        Image(systemName: "chevron.down")
                            .rotationEffect(.degrees(model.panel.isExpanded ? 0 : -90))
                            .foregroundStyle(.secondary)
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .disabled(holding)
                .accessibilityHint(model.panel.isExpanded ? "Collapse" : "Expand")
                if model.panel.isExpanded {
                    CDControls(model: model)
                        .transition(.opacity.combined(with: .move(edge: .top)))
                }
            }
        }
    }
}

struct CDControls: View {
    let model: RemoteModel
    @State private var track = ""
    @FocusState private var trackFocused: Bool
    private let columns = Array(repeating: GridItem(.flexible(), spacing: 10), count: 3)

    var body: some View {
        VStack(spacing: 10) {
            LazyVGrid(columns: columns, spacing: 10) {
                tapKey("backward.end.fill", "Previous track", .cdTrack(next: false)) { model.cdTrack(next: false) }
                tapKey("playpause.fill", "Play or pause", .cdPlayback(.playPause)) { model.cdPlayback(.playPause) }
                tapKey("forward.end.fill", "Next track", .cdTrack(next: true)) { model.cdTrack(next: true) }
                HoldableKey(key: .seekBackward, model: model) {
                    Image(systemName: "backward.fill").font(.title3)
                }
                .accessibilityLabel("Rewind, hold")
                tapKey("stop.fill", "Stop", .cdPlayback(.stop)) { model.cdPlayback(.stop) }
                HoldableKey(key: .seekForward, model: model) {
                    Image(systemName: "forward.fill").font(.title3)
                }
                .accessibilityLabel("Fast forward, hold")
                ForEach([PlaybackAction.timeDisplay, .repeatMode, .discSkip], id: \.self) { action in
                    Button(action.label) { model.cdPlayback(action) }
                        .buttonStyle(RemoteKeyStyle())
                        .disabled(!model.isEnabled(.cdPlayback(action)))
                }
            }
            HStack(spacing: 10) {
                TextField("TRACK #", text: $track)
                    .keyboardType(.numberPad)
                    .focused($trackFocused)
                    .multilineTextAlignment(.center)
                    .font(.title3.monospacedDigit())
                    .frame(height: 52)
                    .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 12))
                    .disabled(!model.isEnabled(.cdTrackSelect))
                    .onChange(of: track) { _, text in
                        let digits = String(text.filter(\.isASCII).filter(\.isNumber).prefix(2))
                        if digits != text { track = digits }
                    }
                    .accessibilityLabel("Track number, \(model.trackRange.lowerBound) to \(model.trackRange.upperBound)")
                Button("GO") {
                    // Only GO sends; typing never does.
                    guard let number = model.validTrack(track) else { return }
                    trackFocused = false
                    model.selectTrack(number)
                }
                .buttonStyle(RemoteKeyStyle())
                .frame(width: 96)
                .disabled(!model.isEnabled(.cdTrackSelect) || model.validTrack(track) == nil)
            }
        }
    }

    private func tapKey(_ symbol: String, _ label: String, _ control: Control,
                        action: @escaping () -> Void) -> some View {
        Button(action: action) { Image(systemName: symbol).font(.title3) }
            .buttonStyle(RemoteKeyStyle())
            .disabled(!model.isEnabled(control))
            .accessibilityLabel(label)
    }
}

struct SectionTitle: View {
    let text: String
    init(_ text: String) { self.text = text }

    var body: some View {
        Text(text.uppercased())
            .font(.caption.weight(.semibold))
            .tracking(1)
            .foregroundStyle(.secondary)
    }
}
