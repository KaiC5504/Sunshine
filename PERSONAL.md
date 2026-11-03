# Personal Sunshine + Moonlight setup

Personal forks for streaming a headless Mac Mini to Windows laptops:

- Sunshine (Mac host): https://github.com/KaiC5504/Sunshine, branch `personal`
- Moonlight (Windows client): https://github.com/KaiC5504/moonlight-qt, branch `personal`

## What the forks add

| Feature | Where |
|---|---|
| Cursor no longer sticks at the top left after the display changes (already fixed upstream, not yet in a stable release) | Sunshine |
| Cursor shows again after typing (ScreenCaptureKit capture, upstream PR 5511 plus the maintainer's requested fixes) | Sunshine |
| Copy and paste text and images both ways while streaming | Sunshine + Moonlight |
| Copy and paste files both ways, or drag files onto the stream window (512 MB per copy, folders are skipped) | Sunshine + Moonlight |
| "In use by …" badge on the Mac Mini tile when another paired device is streaming | Sunshine + Moonlight |
| Update notices from these forks; Moonlight installs its own updates without a SmartScreen warning | both |

Each device needs two switches in Sunshine before the new features work for it (see step 4).

## 1. Signing (once)

Signed and notarized builds keep the Mac's Screen Recording and Accessibility permissions across updates.
The build signs with the same App Store Connect API key and `CERT_KEY_B64` signing key as the iOS TestFlight
builds, so nothing is needed on the Mac and there are no passwords to make.

1. **Secrets.** The `testflight` environment already exists on this repo, limited to the `personal` branch. Add the five
   TestFlight secrets to it, the same values as every iOS app, in your own terminal:

   ```powershell
   pwsh C:\Users\KaiChuan\.claude\skills\ios-testflight\scripts\set-secrets.ps1 -File <keys file> -Repo KaiC5504/Sunshine
   ```

   Or add them by hand at https://github.com/KaiC5504/Sunshine/settings/environments > **testflight** >
   **Add environment secret**: `APP_STORE_CONNECT_ISSUER_ID`, `APP_STORE_CONNECT_KEY_IDENTIFIER`,
   `APP_STORE_CONNECT_PRIVATE_KEY`, `CERT_KEY_B64`, `SIGNING_NAME`.
2. **Developer ID certificate.** Apps outside the App Store need a *Developer ID Application* certificate, which only
   the Account Holder can create on the developer portal (the API key can't). The next build after step 1 makes the
   certificate request from `CERT_KEY_B64` for you:
   1. Open the newest run at https://github.com/KaiC5504/Sunshine/actions/workflows/personal-macos.yml. It shows a
      warning that no Developer ID certificate matches. Under **Artifacts**, download `developer-id-csr` and unzip it.
   2. https://developer.apple.com/account/resources/certificates/add > **Developer ID Application** > Continue >
      **G2 Sub-CA (Xcode 11.4.1 or later)** > Choose File: `DeveloperID.certSigningRequest` > Continue.
      There's no need to download the certificate.

   From then on every build finds the certificate through the API key and signs with it. Releases are also notarized
   with the same key.

## 2. Publish releases

Every push to `personal` builds and tests both apps. To publish a release:

- Sunshine: https://github.com/KaiC5504/Sunshine/actions/workflows/personal-macos.yml > Run workflow > tick
  **Publish a signed GitHub release**. Notarization makes this take a while.
- Moonlight: https://github.com/KaiC5504/moonlight-qt/actions/workflows/personal-windows.yml > Run workflow > tick
  **Publish a GitHub release**.

Or from a terminal:

```bash
gh workflow run personal-macos.yml -R KaiC5504/Sunshine -f release=true
gh workflow run personal-windows.yml -R KaiC5504/moonlight-qt -f release=true
```

## 3. Install

**Mac Mini (Sunshine)**

1. If Sunshine came from Homebrew: `brew services stop sunshine && brew uninstall sunshine`. Otherwise quit it from the menu bar.
2. Download `Sunshine-macOS-arm64.dmg` (Apple silicon only; the Intel build was dropped to halve build time) from
   https://github.com/KaiC5504/Sunshine/releases/latest, open it and drag Sunshine into Applications.
3. Open Sunshine and allow Screen & System Audio Recording and Accessibility when asked.
   The first file copied from Windows also asks for access to the Downloads folder.
4. System Settings > General > Login Items: add Sunshine so it starts after a reboot.

Your settings and paired devices in `~/.config/sunshine` stay as they are.

**Windows laptops (Moonlight)** — yours and your friend's. In PowerShell:

```powershell
$f = "$env:TEMP\MoonlightSetup.exe"; Invoke-WebRequest https://github.com/KaiC5504/moonlight-qt/releases/latest/download/MoonlightSetup.exe -OutFile $f; & $f
```

Downloading with PowerShell instead of a browser avoids the SmartScreen warning. The installer replaces official
Moonlight and keeps paired hosts. Later updates show as an update button in Moonlight's top bar; clicking it installs
the new version.

## 4. Turn the features on per device

Open the Sunshine web UI (`https://<mac-ip>:47990`) > Troubleshooting > the paired device list. Each device has:

- **Clipboard**: copy and paste (text, images and files) while that device streams.
- **See who is connected**: shows the "In use by …" badge in that device's Moonlight. Turn this on for your laptop only.

Both are off for every device until you turn them on. The badge shows the device name entered when it was paired.

## How it behaves

- Only things copied after a stream starts are shared. Copying something on one side replaces the clipboard on the other.
- Passwords copied from password managers on the Mac are never shared.
- Files from the Mac go to `Downloads\Moonlight` on Windows and are put on the Windows clipboard, so Ctrl+V in Explorer pastes them.
- Files from Windows go to `~/Downloads/Moonlight` on the Mac and are put on the Mac clipboard, so Cmd+V in Finder pastes them.
- The badge updates every few seconds while Moonlight shows the computer list.

## Building on the Mac without a release

`personal-mac/build-sunshine-mac.sh` in this repo builds the `personal` branch on the Mac and installs it ad hoc
signed. macOS then asks for the permissions again after every rebuild, so prefer signed releases.
