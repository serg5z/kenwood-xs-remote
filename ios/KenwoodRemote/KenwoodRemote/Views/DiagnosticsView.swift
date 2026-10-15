import SwiftUI

/// Everything the Gateway reported in the last read, for checking the live
/// deployment (gates, track range, CD state) without the Web Remote.
struct DiagnosticsView: View {
    let model: RemoteModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List {
                Section("Connection") {
                    row("Host", GatewayConfiguration.woodland.host)
                    row("State", model.connection.title)
                    if let detail = model.connection.detail { row("Detail", detail) }
                    row("Last successful read", model.lastSuccess?.formatted(date: .omitted, time: .standard) ?? "—")
                    if let issue = model.snapshot?.controlInfoIssue {
                        row("Control info", ConnectionState(issue).detail ?? ConnectionState(issue).title)
                    }
                }
                if let status = model.snapshot?.status.status {
                    Section("XS8 Service") {
                        ForEach(["state", "monitor", "stop_owed", "context_may_be_open",
                                 "cd_state", "cd_release_owed", "cd_last_result"], id: \.self) { key in
                            if let value = status[key] { row(key, value) }
                        }
                    }
                }
                if let receiver = model.snapshot?.status.receiver {
                    Section("Receiver observations") {
                        observation("Source", receiver.source)
                        observation("TAPE2 MONITOR", receiver.monitor)
                        row("Integrity", receiver.integrity)
                    }
                }
                if let power = model.snapshot?.status.power {
                    Section("Power observation") {
                        row("Value", power.value)
                        row("Freshness", power.freshness)
                        row("Evidence", power.evidence ?? "—")
                        row("Age", power.ageMs.map { SourceIndicator.age($0) } ?? "—")
                    }
                }
                if let changer = model.snapshot?.status.cdChanger {
                    Section("CD changer (diagnostic)") {
                        row("Presence", changer.value)
                        row("Last probe", changer.lastProbe ?? "—")
                    }
                }
                if let capabilities = model.snapshot?.capabilities {
                    Section {
                        ForEach(OperationName.allCases, id: \.self) { name in
                            if let operation = capabilities.operation(name) {
                                VStack(alignment: .leading, spacing: 2) {
                                    HStack {
                                        Text(name.rawValue).font(.body.monospaced())
                                        Spacer()
                                        Text(operation.enabled ? "enabled" : "off")
                                            .foregroundStyle(operation.enabled ? .green : .secondary)
                                    }
                                    if let details = details(operation) {
                                        Text(details).font(.caption).foregroundStyle(.secondary)
                                    }
                                }
                            }
                        }
                    } header: {
                        Text("Gateway capabilities \(capabilities.apiVersion ?? "")")
                    } footer: {
                        Text("Advertised by the Gateway for this session. This build sends no controls.")
                    }
                }
            }
            .navigationTitle("Details")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { Button("Done") { dismiss() } }
            .refreshable { await model.refresh() }
        }
    }

    private func row(_ label: String, _ value: String) -> some View {
        LabeledContent(label) { Text(value).font(.callout.monospaced()).multilineTextAlignment(.trailing) }
    }

    private func observation(_ label: String, _ observation: ReceiverObservation) -> some View {
        row(label, "\(observation.value) · \(observation.freshness)"
            + (observation.ageMs.map { " · \(SourceIndicator.age($0))" } ?? ""))
    }

    private func details(_ operation: Capabilities.Operation) -> String? {
        var parts: [String] = []
        if let actions = operation.availableActions { parts.append(actions.joined(separator: ", ")) }
        if let directions = operation.availableDirections { parts.append(directions.joined(separator: ", ")) }
        if let sources = operation.availableSources { parts.append(sources.joined(separator: ", ")) }
        if let min = operation.minTrack, let max = operation.maxTrack { parts.append("tracks \(min)–\(max)") }
        if let interval = operation.minIntervalMs { parts.append("interval \(interval) ms") }
        if let maxHold = operation.maxHoldMs { parts.append("max hold \(maxHold) ms") }
        if operation.toggleAvailable == true { parts.append("toggle") }
        return parts.isEmpty ? nil : parts.joined(separator: " · ")
    }
}
