import SwiftUI

/// A remote key. The fill carries observed state and is independent of whether
/// the key is enabled, so a disabled source key still shows the observation.
struct RemoteKeyStyle: ButtonStyle {
    enum Fill { case plain, observed, lastKnown }

    var fill: Fill = .plain
    var height: CGFloat = 52

    @Environment(\.isEnabled) private var isEnabled

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
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
            .scaleEffect(configuration.isPressed ? 0.97 : 1)
    }

    private var background: Color {
        switch fill {
        case .plain: Color(.secondarySystemBackground)
        case .observed: .accentColor
        case .lastKnown: .accentColor.opacity(0.18)
        }
    }

    private var foreground: Color {
        switch fill {
        case .observed: .white
        case .lastKnown: .accentColor
        case .plain: isEnabled ? .primary : .secondary
        }
    }

    private var border: Color {
        switch fill {
        case .plain: Color(.separator)
        case .observed: .clear
        case .lastKnown: .accentColor.opacity(0.7)
        }
    }
}
