# Kenwood Remote for iOS

A personal SwiftUI remote for the KR-V6070. It talks only to the existing Gateway at `https://woodland-pi.local`. The Raspberry Pi remains the only authority for hardware control and safety. The app never sends XS8 bytes.

**Status: Phase 2, read-only.** The app reads `GET /api/v1/status`, `/session` and `/capabilities` only. The main screen shows the agreed layout with every control disabled and with no action. Normal polling (every 5 s while the app is in the foreground) and pull-to-refresh only read; neither sends SYNC.

## Setup

Requirements: Xcode 26 or later, [XcodeGen](https://github.com/yonaskolb/XcodeGen), an iOS 17+ device, and the Apple team set in `project.yml`.

1. Put the private CA's **public** certificate into the untracked `Local/` folder in DER form. Never copy `ca.key`.

   ```sh
   mkdir -p Local
   openssl x509 -in ~/kenwood-certs/kenwood-ca.crt -outform der -out Local/KenwoodCA.der
   shasum -a 256 Local/KenwoodCA.der   # must equal GatewayConfiguration.woodland.caSHA256
   ```

   The app refuses to connect if the bundled file's SHA-256 does not match the configured fingerprint.

2. Generate the Xcode project. The project file is generated and untracked.

   ```sh
   xcodegen generate
   ```

   If XcodeGen is a symlink to an install with a `share/xcodegen/SettingPresets` folder, run the real binary, for example `~/.local/share/xcodegen/bin/xcodegen generate`. Without its presets, the generated targets have no product name.

3. Run the tests. They use a stubbed network layer and never contact the Pi.

   ```sh
   xcodebuild -project KenwoodRemote.xcodeproj -scheme KenwoodRemote \
     -destination 'platform=iOS Simulator,name=iPhone 17 Pro' -derivedDataPath build test
   ```

4. Run on the iPhone from Xcode. On first launch, allow Local Network access and enter the nginx Basic Auth account. The app stores it in the Keychain, available only while the device is unlocked, on this device only.

## Security model

- **TLS:** the bundled private CA is the **only** trust anchor (`SecTrustSetAnchorCertificatesOnly`), with the SSL policy for `woodland-pi.local`. A trust profile installed on the iPhone is neither needed nor used. Any other host is refused.
- **Basic Auth:** the app answers nginx's challenge only for the Gateway host over a verified HTTPS connection. A repeated challenge is treated as a rejected password, so it never loops.
- **Session cookie:** the app manages the `xs8_session` cookie by hand. URLSession cookie storage is off, and the app sends exactly one `Cookie` header. The cookie is kept in memory only.
- **No hidden traffic:** URLSession is ephemeral, with no cache and no `waitsForConnectivity`, so a request never goes out late.
- **Read endpoints only:** the client can only build body-less GETs. It sends no `Origin` and no CSRF token, and it does not decode or keep the session's CSRF token.

## Layout

```text
KenwoodRemote/
  App/          KenwoodRemoteApp (inert model while hosting unit tests)
  Gateway/      configuration, pinned trust, URLSession transport, models, GatewayClient (actor)
  Credentials/  Keychain credential store
  State/        RemoteModel (@Observable), observation → display rules, panel state
  Views/        main screen, account, details (live capabilities and STATUS)
KenwoodRemoteTests/
  Fixtures/     JSON produced by xs8_gateway.py itself; throwaway test PKI (no real certificates)
```
