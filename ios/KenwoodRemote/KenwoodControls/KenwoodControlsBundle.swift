import AppIntents
import SwiftUI
import WidgetKit

/// Control Center controls: open the app, a status diagnostic, MUTE and volume steps.
@main
struct KenwoodControlsBundle: WidgetBundle {
    var body: some Widget {
        OpenKenwoodControl()
        GatewayStatusControl()
        MuteControl()
        VolumeUpControl()
        VolumeDownControl()
    }
}

/// Stateless: a tap sends one MUTE toggle; no mute state is shown.
struct MuteControl: ControlWidget {
    static let kind = "local.woodland.KenwoodRemote.mute"

    var body: some ControlWidgetConfiguration {
        StaticControlConfiguration(kind: Self.kind) {
            ControlWidgetButton(action: MuteIntent()) {
                Label("Kenwood MUTE", systemImage: "speaker.slash")
            }
        }
        .displayName("Kenwood MUTE")
        .description("Sends one MUTE toggle. Requires unlocking.")
    }
}

struct VolumeUpControl: ControlWidget {
    static let kind = "local.woodland.KenwoodRemote.volume-up"

    var body: some ControlWidgetConfiguration {
        StaticControlConfiguration(kind: Self.kind) {
            ControlWidgetButton(action: VolumeStepIntent(direction: .up)) {
                Label("Kenwood Volume +", systemImage: "speaker.plus")
            }
        }
        .displayName("Kenwood Volume +")
        .description("Sends one volume step up. Requires unlocking.")
    }
}

struct VolumeDownControl: ControlWidget {
    static let kind = "local.woodland.KenwoodRemote.volume-down"

    var body: some ControlWidgetConfiguration {
        StaticControlConfiguration(kind: Self.kind) {
            ControlWidgetButton(action: VolumeStepIntent(direction: .down)) {
                Label("Kenwood Volume −", systemImage: "speaker.minus")
            }
        }
        .displayName("Kenwood Volume −")
        .description("Sends one volume step down. Requires unlocking.")
    }
}

struct OpenKenwoodControl: ControlWidget {
    static let kind = "local.woodland.KenwoodRemote.open"

    var body: some ControlWidgetConfiguration {
        StaticControlConfiguration(kind: Self.kind) {
            ControlWidgetButton(action: OpenKenwoodIntent(target: .remote)) {
                Label("Kenwood", systemImage: "hifispeaker.fill")
            }
        }
        .displayName("Open Kenwood")
        .description("Opens the Kenwood remote.")
    }
}

/// Diagnostic only: `GET /api/v1/status`, nothing transmitted to the receiver.
struct GatewayStatusControl: ControlWidget {
    static let kind = "local.woodland.KenwoodRemote.status"

    var body: some ControlWidgetConfiguration {
        StaticControlConfiguration(kind: Self.kind) {
            ControlWidgetButton(action: GatewayStatusIntent()) {
                Label("Kenwood Status", systemImage: "stethoscope")
            }
        }
        .displayName("Kenwood Status (diagnostic)")
        .description("Reads the Gateway status. Sends nothing to the receiver.")
    }
}
