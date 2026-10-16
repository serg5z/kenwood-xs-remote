# Kenwood Remote for iOS

A personal SwiftUI remote for the KR-V6070. It talks only to the existing Gateway at `https://woodland-pi.local`. The Raspberry Pi remains the only authority for hardware control and safety. The app never sends XS8 bytes.

**Status: v1 with controls, awaiting attended physical validation.** The app polls `GET /api/v1/status`, `/session` and `/capabilities` every 5 s while it is in the foreground. Pull-to-refresh also only reads; SYNC is a separate explicit button.

It enables only what the Gateway's capabilities advertise:

- One-click POWER, with the explicit ON/OFF choice when a toggle is refused or unavailable.
- The six sources and TAPE2 MONITOR.
- VOL± as a tap or press-and-hold, and MUTE.
- CD: PREV/NEXT, PLAY/PAUSE, STOP, held FF/REW, TIME DISP., REPEAT, DISC SKIP, and Track # + GO.
- SYNC.

## Control model

- **One request at a time.** Reads and controls share one serial lane in `GatewayClient`. A tap during a running control is dropped, never queued. Client-side spacing mirrors the Gateway's, which stays authoritative.
- **Exact requests.** Each command is one documented endpoint with an allowlisted body of at most 64 bytes (`GatewayControl.swift`). The headers are the exact `Origin`, `Content-Type: application/json`, the session cookie and a one-use `X-CSRF-Token`. The app clears the token before sending and replaces it only with the reply's successor or a fresh `GET /session`.
- **Outcome policy.** No request is ever retried.
  - HTTP 200 means TX VERIFIED, not that the receiver acted.
  - An invalid Origin, and other clean refusals (400, 401, 409 refusals, 429, 501, the Gateway's 503), mean NOTHING SENT.
  - An invalid CSRF token, 502/504, an unreadable 200, `physical_check_required` and ambiguous network errors mean UNCERTAIN.
  - `FAULT_LATCHED` / `CD_FAULT_LATCHED` mean FAULT, and the affected controls stay off.
- **Observed state only.** Indicators change only from Gateway observations. After a source or MONITOR control, observations older than the reply's `sync.since_mono_ns` are not shown.
- **Holds** (`/volume/hold`, `/cd/seek`):
  - A volume press shorter than `hold_delay_ms` is one step; a longer one is a hold, never both.
  - A hold is START, then a HEARTBEAT every `heartbeat_ms`, then exactly one RELEASE.
  - RELEASE is sent when the finger lifts, the touch is cancelled, the app becomes inactive, the key's view disappears, the network path drops, a heartbeat fails, or the advertised maximum is reached.
  - A START still waiting for its turn is not sent once the key is released.
  - An uncertain START gets one stop-only RELEASE without `hold_id`.
  - The release runs inside an OS background task. The service lease (1.2 s), the Gateway's 1.5 s browser timeout and the maxima remain the backstops.

## Control Center (`KenwoodControls`, iOS 18+)

A WidgetKit extension, embedded in the app, provides:

- **Control Center buttons** (`ControlWidgetButton`): Open Kenwood (`OpenIntent`); Kenwood Status, a diagnostic that sends only `GET /api/v1/status`; MUTE; Volume +; Volume −.
  - Volume is one step per tap. There is no hold.
  - Every hardware action requires an unlocked device (`authenticationPolicy = .requiresAuthentication`). No state is ever shown on the buttons.

How a control tap runs (`Shared/ExtensionCommandRunner.swift`):

1. It reads the password from the shared keychain group `TWMHP49N84.local.woodland.KenwoodRemote.shared`. The app moves its credentials into this group once, copying before deleting.
2. It runs `GET /session`: that confirms or renews the session cookie and fetches a fresh one-use token and the Gateway's current availability.
3. It sends one POST, using the app's outcome policy, never retried.

The rules around that:

- One tap at a time per extension process. A tap that arrives meanwhile is dropped.
- **Separate session:** the extension keeps its own Gateway session, so it never uses up the app's CSRF tokens. Its cookie lives in the extension-only keychain group.
- **Cookie lifetime:** 25 minutes from when the Gateway issued it, because the Gateway's 30 minutes start at creation and use doesn't extend them. No CSRF token is ever stored.
- **Results:** each tap's command, timing and result are saved for Kenwood → Details → Control Center.

## Platform findings (iOS 26.7.1, iPhone 13 mini)

Recorded from the Control Center, Lock Screen and snippet spikes (2026-10-15). All spike code has been removed.

- **Control Center buttons** work.
  - Shared keychain, pinned TLS and Local Network access all work from the extension.
  - iOS reuses the extension process between taps (run #10 observed). A status read typically completes in about 574 ms.
  - From the Lock Screen, iOS asks for Face ID before running an action.
  - Keychain sharing works with the existing wildcard team profile (`keychain-access-groups TWMHP49N84.*`). No account changes were needed.
- **Lock Screen widgets:** dropped.
  - A four-button `accessoryRectangular` widget (`Button(intent:)`) triggered each button's own action reliably, but the targets were too small for comfortable use.
  - A single-tap "Open Kenwood" launcher widget wasn't useful enough to keep.
- **Interactive snippet from a control** (`ShowsSnippetIntent` / `SnippetIntent`): negative result.
  - The control's intent ran and returned `.result(snippetIntent:)`, the documented iOS 26 mechanism, but iOS never asked for the snippet itself and no panel appeared.
  - Apple's documentation doesn't say that control actions present snippets. Closed.
- **"Failed to load Asset / Could not load asset"** while adding a widget came from the Photos-based Lock Screen wallpaper, not from the widget. The sysdiagnose showed `PhotosPosterProvider: Failed to load layer stack from wallpaper … Code=260` at every editing session, and `assetsd` exited at the same time. The widget itself registered, rendered and ran its intents.
- **Widget `ForEach` IDs must be Encodable.** Widget views are archived, so `ForEach(…, id: \.self)` over an `AppEnum` (which isn't `Codable`) triggers a SwiftUI fault `ID type is not Encodable`.
- **Continuous press-and-hold** is not possible outside the app on iOS 26.
  - Widget, Live Activity, control and snippet buttons run an intent only after a completed tap. They report no press, release or repeat.
  - The system Now Playing FF/REW hold is reserved for the app whose audio session is playing.
  - The NowPlaying `RemoteMediaSession` API, which has seek begin and end, needs iOS 27.
- **Collecting device logs:** `xcrun devicectl device sysdiagnose --device <id>` (confirm on stdin). Then use `/usr/bin/log show --archive …/system_logs.logarchive`. In zsh, a bare `log` is a built-in command, not the macOS tool.

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
- **CSRF token:** the session's token stays inside `GatewayClient`. It is never logged and never shown in the UI.

## Layout

```text
KenwoodRemote/
  App/          KenwoodRemoteApp (inert model while hosting unit tests)
  Gateway/      configuration, pinned trust, URLSession transport, models, control commands and
                outcome policy, GatewayClient (actor, serial lane)
  Credentials/  Keychain credential store
  State/        RemoteModel (@Observable) and its controls/holds, observation → display rules
  Views/        main screen, account, details (live capabilities and STATUS, last Control Center run)
KenwoodControls/  WidgetKit extension: Control Center buttons and their intents
Shared/       compiled into app and extension: OpenIntent, extension command runner,
                extension session-cookie store, Control Center diagnostic record
KenwoodRemoteTests/
  Fixtures/     JSON produced by xs8_gateway.py itself; throwaway test PKI (no real certificates)
```
