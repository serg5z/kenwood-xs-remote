import SwiftUI

/// Main screen, top to bottom: header and POWER, sources, volume, then the
/// contextual panel for the observed source. Read-only build: every control is
/// disabled and has no action.
struct MainRemoteView: View {
    @Bindable var model: RemoteModel
    @Environment(\.scenePhase) private var scenePhase
    @State private var showingCredentials = false
    @State private var showingDetails = false

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 20) {
                    HeaderView(model: model)
                    ReadOnlyBanner()
                    SourceSelector(indicator: model.sourceIndicator,
                                   monitor: model.monitorIndicator)
                    VolumeControls()
                    ContextualPanel(panel: $model.panel)
                }
                .padding(.horizontal, 16)
                .padding(.bottom, 24)
            }
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

private struct ReadOnlyBanner: View {
    var body: some View {
        Label("Read-only preview · controls are not enabled in this build",
              systemImage: "lock")
            .font(.footnote)
            .foregroundStyle(.secondary)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(10)
            .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 10))
    }
}

// MARK: Header

struct HeaderView: View {
    let model: RemoteModel

    var body: some View {
        HStack(alignment: .center, spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Text("KENWOOD").font(.title2.weight(.heavy)).tracking(2)
                Text("KR-V6070").font(.caption.weight(.medium)).foregroundStyle(.secondary)
                ConnectionBadge(state: model.connection)
            }
            Spacer()
            VStack(spacing: 4) {
                PowerButton(indicator: model.powerIndicator)
                Text(model.powerIndicator.shortLabel)
                    .font(.caption2).foregroundStyle(.secondary)
            }
        }
        .padding(.top, 8)
    }
}

struct ConnectionBadge: View {
    let state: ConnectionState

    var body: some View {
        HStack(spacing: 6) {
            Circle().fill(color).frame(width: 8, height: 8)
            Text(state.title).font(.caption.weight(.semibold))
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

    var body: some View {
        Button {} label: {
            Image(systemName: "power")
                .font(.title2.weight(.bold))
                .foregroundStyle(.white)
                .frame(width: 56, height: 56)
                .background(Circle().fill(Color.red.opacity(0.55)))
                .overlay { ring }
        }
        .buttonStyle(.plain)
        .disabled(true)
        .accessibilityLabel("Power, not enabled in this build. \(indicator.label)")
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
    let indicator: SourceIndicator
    let monitor: MonitorIndicator

    private let columns = Array(repeating: GridItem(.flexible(), spacing: 10), count: 3)

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            SectionTitle("Source")
            LazyVGrid(columns: columns, spacing: 10) {
                ForEach(Source.allCases) { source in
                    Button(source.rawValue) {}
                        .buttonStyle(RemoteKeyStyle(fill: fill(for: source)))
                        .disabled(true)
                        .accessibilityValue(accessibility(for: source))
                }
            }
            Text(indicator.summary)
                .font(.footnote)
                .foregroundStyle(.secondary)
            Button {} label: {
                Text(monitorLabel)
            }
            .buttonStyle(RemoteKeyStyle(fill: monitor == .on ? .observed : .plain, height: 44))
            .disabled(true)
        }
    }

    private func fill(for source: Source) -> RemoteKeyStyle.Fill {
        switch indicator.highlight(for: source) {
        case .fresh: .observed
        case .lastKnown: .lastKnown
        case .none: .plain
        }
    }

    private func accessibility(for source: Source) -> String {
        switch indicator.highlight(for: source) {
        case .fresh: "Observed selected"
        case .lastKnown: "Last known selection"
        case .none: ""
        }
    }

    private var monitorLabel: String {
        switch monitor {
        case .on: "TAPE2 MONITOR ON"
        case .off: "TAPE2 MONITOR OFF"
        case .unknown: "TAPE2 MONITOR"
        }
    }
}

// MARK: Volume

struct VolumeControls: View {
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            SectionTitle("Volume")
            HStack(spacing: 10) {
                Button {} label: { Image(systemName: "minus").font(.title2.weight(.bold)) }
                    .accessibilityLabel("Volume down")
                Button("MUTE") {}
                Button {} label: { Image(systemName: "plus").font(.title2.weight(.bold)) }
                    .accessibilityLabel("Volume up")
            }
            .buttonStyle(RemoteKeyStyle(height: 64))
            .disabled(true)
        }
    }
}

// MARK: Contextual panel

struct ContextualPanel: View {
    @Binding var panel: ContextPanelState

    var body: some View {
        if let source = panel.visiblePanel {
            VStack(alignment: .leading, spacing: 12) {
                Button {
                    withAnimation(.snappy) { panel.toggle() }
                } label: {
                    HStack {
                        SectionTitle("\(source.rawValue) controls")
                        Spacer()
                        Image(systemName: "chevron.down")
                            .rotationEffect(.degrees(panel.isExpanded ? 0 : -90))
                            .foregroundStyle(.secondary)
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityHint(panel.isExpanded ? "Collapse" : "Expand")
                if panel.isExpanded {
                    CDControls()
                        .transition(.opacity.combined(with: .move(edge: .top)))
                }
            }
        }
    }
}

struct CDControls: View {
    @State private var track = ""
    private let columns = Array(repeating: GridItem(.flexible(), spacing: 10), count: 3)

    var body: some View {
        VStack(spacing: 10) {
            LazyVGrid(columns: columns, spacing: 10) {
                key("backward.end.fill", "Previous track")
                key("playpause.fill", "Play or pause")
                key("forward.end.fill", "Next track")
                key("backward.fill", "Rewind (hold)")
                key("stop.fill", "Stop")
                key("forward.fill", "Fast forward (hold)")
                Button("TIME DISP.") {}
                Button("REPEAT") {}
                Button("DISC SKIP") {}
            }
            HStack(spacing: 10) {
                TextField("TRACK #", text: $track)
                    .keyboardType(.numberPad)
                    .multilineTextAlignment(.center)
                    .font(.title3.monospacedDigit())
                    .frame(height: 52)
                    .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 12))
                Button("GO") {}
                    .frame(width: 96)
            }
        }
        .buttonStyle(RemoteKeyStyle())
        .disabled(true)
    }

    private func key(_ symbol: String, _ label: String) -> some View {
        Button {} label: { Image(systemName: symbol).font(.title3) }
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
