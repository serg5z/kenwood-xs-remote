import AppIntents

/// Where "Open Kenwood" lands. One destination for now.
enum KenwoodDestination: String, AppEnum {
    case remote

    static let typeDisplayRepresentation: TypeDisplayRepresentation = "Kenwood screen"
    static let caseDisplayRepresentations: [KenwoodDestination: DisplayRepresentation] = [.remote: "Remote"]
}

/// Opens the app (OpenIntent). Compiled into both the app and the Control
/// Center extension, as the system runs an opening intent in the app.
/// It sends nothing to the Gateway.
struct OpenKenwoodIntent: OpenIntent {
    static let title: LocalizedStringResource = "Open Kenwood"
    static let description = IntentDescription("Opens the Kenwood remote.")

    @Parameter(title: "Screen")
    var target: KenwoodDestination

    init() {}

    init(target: KenwoodDestination) {
        self.target = target
    }

    func perform() async throws -> some IntentResult { .result() }
}
