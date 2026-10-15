import SwiftUI

/// The face of a remote key. The fill carries observed state and is
/// independent of whether the key is enabled, so a disabled source key still
/// shows the observation.
struct KeyFace: ViewModifier {
    enum Fill { case plain, observed, lastKnown, active }

    var fill: Fill = .plain
    var height: CGFloat = 52
    var enabled = true
    var pressed = false

    func body(content: Content) -> some View {
        content
            .font(.subheadline.weight(.semibold))
            .lineLimit(1)
            .minimumScaleFactor(0.7)
            .frame(maxWidth: .infinity, minHeight: height)
            .foregroundStyle(foreground)
            .background(background, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            .overlay {
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .strokeBorder(border, style: StrokeStyle(lineWidth: fill == .lastKnown ? 1.5 : 1,
                                                             dash: fill == .lastKnown ? [5, 3] : []))
            }
            .scaleEffect(pressed ? 0.96 : 1)
            .animation(.easeOut(duration: 0.08), value: pressed)
    }

    private var background: Color {
        switch fill {
        case .plain: pressed ? Color(.tertiarySystemFill) : Color(.secondarySystemBackground)
        case .observed: .accentColor
        case .lastKnown: .accentColor.opacity(0.18)
        case .active: .orange
        }
    }

    private var foreground: Color {
        switch fill {
        case .observed, .active: .white
        case .lastKnown: .accentColor
        case .plain: enabled ? .primary : Color(.tertiaryLabel)
        }
    }

    private var border: Color {
        switch fill {
        case .plain: Color(.separator)
        case .observed, .active: .clear
        case .lastKnown: .accentColor.opacity(0.7)
        }
    }
}

struct RemoteKeyStyle: ButtonStyle {
    var fill: KeyFace.Fill = .plain
    var height: CGFloat = 52

    @Environment(\.isEnabled) private var isEnabled

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .modifier(KeyFace(fill: fill, height: height, enabled: isEnabled,
                              pressed: configuration.isPressed))
    }
}

/// A press-and-hold key (VOL±, FF/REW). Press and release go to the model,
/// which decides tap versus hold; a cancelled touch never counts as a tap.
struct HoldableKey<Label: View>: View {
    let key: HoldKey
    let model: RemoteModel
    var height: CGFloat = 52
    @ViewBuilder let label: () -> Label

    @GestureState private var pressing = false

    /// A touch that wandered this far is treated as cancelled, not a tap.
    private static var slop: CGFloat { 40 }

    var body: some View {
        let holding = model.activeHold?.key == key
        let enabled = model.isEnabled(key) || holding
        label()
            .modifier(KeyFace(fill: holding ? .active : .plain, height: height,
                              enabled: enabled, pressed: pressing))
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .updating($pressing) { _, state, _ in state = true }
                    .onEnded { value in
                        let moved = hypot(value.translation.width, value.translation.height)
                        model.pressEnded(key, completed: moved <= Self.slop)
                    },
                isEnabled: enabled)
            .onChange(of: pressing) { _, isPressing in
                if isPressing {
                    model.pressBegan(key)
                } else {
                    // After onEnded has had its turn: then this only ever
                    // cancels (a gesture SwiftUI cancelled has no onEnded).
                    Task { @MainActor in
                        await Task.yield()
                        model.pressEnded(key, completed: false)
                    }
                }
            }
            .onDisappear {
                model.pressEnded(key, completed: false)
                if model.activeHold?.key == key { model.interrupt(reason: "VIEW CLOSED") }
            }
            .accessibilityElement(children: .combine)
            .accessibilityAddTraits(.isButton)
    }
}
