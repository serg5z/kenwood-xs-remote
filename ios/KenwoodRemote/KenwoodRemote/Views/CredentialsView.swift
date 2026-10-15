import SwiftUI

struct CredentialsView: View {
    let model: RemoteModel
    @Environment(\.dismiss) private var dismiss
    @State private var username = ""
    @State private var password = ""
    @State private var error: String?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("User", text: $username)
                        .textContentType(.username)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    SecureField("Password", text: $password)
                        .textContentType(.password)
                } header: {
                    Text(GatewayConfiguration.woodland.host)
                } footer: {
                    Text("The nginx Basic Auth account. Stored in this iPhone's Keychain (this device only) and sent only over HTTPS after the server certificate passes the private-CA check.")
                }
                if model.connection == .authenticationRejected {
                    Section { Label("The Pi rejected the saved password.", systemImage: "exclamationmark.triangle") }
                }
                if let error {
                    Section { Text(error).foregroundStyle(.red) }
                }
                if model.savedUsername != nil {
                    Section {
                        Button("Forget saved credentials", role: .destructive) {
                            do {
                                try model.forgetCredentials()
                                username = ""
                                password = ""
                            } catch {
                                self.error = "Keychain delete failed: \(error)"
                            }
                        }
                    }
                }
            }
            .navigationTitle("Account")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") { save() }
                        .disabled(username.isEmpty || password.isEmpty)
                }
            }
            .onAppear { username = model.savedUsername ?? "" }
        }
    }

    private func save() {
        do {
            try model.saveCredentials(BasicCredentials(username: username, password: password))
            password = ""
            dismiss()
            Task { await model.refresh() }
        } catch {
            self.error = "Keychain save failed: \(error)"
        }
    }
}
