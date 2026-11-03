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

## Without a screen: SSH

Turn these on once from the Mac's Terminal (Cmd+Space, type `Terminal`, Return):

```bash
sudo systemsetup -setremotelogin on        # SSH. If it asks for Full Disk Access, use the next line instead.
sudo launchctl load -w /System/Library/LaunchDaemons/ssh.plist
sudo launchctl load -w /System/Library/LaunchDaemons/com.apple.screensharing.plist   # VNC fallback when Sunshine is down
sudo pmset -a sleep 0 womp 1 autorestart 1  # never sleep, wake on network, restart after a power cut
```

Sunshine only runs once someone is logged in, so a reboot leaves it off until you log in. Automatic login
(System Settings > Users & Groups) fixes that, but macOS doesn't offer it while FileVault is on.

From Windows, `ssh <mac user>@<mac name>.local`. To log in with a key instead of the password, in PowerShell:

```powershell
ssh-keygen -t ed25519
Get-Content $env:USERPROFILE\.ssh\id_ed25519.pub | ssh <mac user>@<mac name>.local "mkdir -p ~/.ssh && chmod 700 ~/.ssh && cat >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys"
```

Keep SSH on the home network: don't forward port 22 on the router.

**Pairing a device over SSH.** The web UI works from any laptop's browser at `https://<mac-ip>:47990`, so no screen
is needed for pairing. Sending a PIN from that address needs it in Sunshine's allowed origins, once:
`echo 'csrf_allowed_origins = https://<mac-ip>:47990' >> ~/.config/sunshine/sunshine.conf`, then restart Sunshine. To do it from SSH instead, fetch the helper once on the Mac:

```bash
curl -fsSLO https://raw.githubusercontent.com/KaiC5504/Sunshine/personal/personal-mac/sunshine-pair.sh
```

Start pairing in Moonlight, leave its PIN showing, then run `bash sunshine-pair.sh` on the Mac and type the PIN.
Moonlight's "A pairing session with this uniqueid already exists (Error 409)" means an earlier pairing attempt
from that device is still waiting for its PIN; `bash sunshine-pair.sh --cancel` clears it (it also clears itself
after 5 minutes).

## How it behaves

- Only things copied after a stream starts are shared. Copying something on one side replaces the clipboard on the other.
- Passwords copied from password managers on the Mac are never shared.
- Files from the Mac go to `Downloads\Moonlight` on Windows and are put on the Windows clipboard, so Ctrl+V in Explorer pastes them.
- Files from Windows go to `~/Downloads/Moonlight` on the Mac and are put on the Mac clipboard, so Cmd+V in Finder pastes them.
- The badge updates every few seconds while Moonlight shows the computer list.

## Building on the Mac

Day-to-day changes build on the Mac itself: only changed files recompile, and the result is signed with the same
Developer ID as the releases, so macOS keeps Sunshine's permissions. A change is running a few minutes after it is
pushed, instead of the half hour a GitHub Actions build takes. Releases still come from GitHub Actions.

Once:

1. Install Homebrew (asks for the Mac password):
   `ssh -t mac '/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"'`
2. Clone the fork to `~/src/sunshine-personal` on the Mac with submodules, then let it accept pushes:
   `git config receive.denyCurrentBranch updateInstead`. On the PC: `git remote add mac mac:src/sunshine-personal`.
3. Import the signing key, pasting `CERT_KEY_B64` from the password manager when asked:
   `Get-Clipboard | ssh mac 'bash ~/src/sunshine-personal/personal-mac/setup-signing.sh'` in PowerShell.
   It goes into its own keychain, `sunshine-signing`, whose password is in `~/.config/sunshine-build/`.

Each change:

```bash
git push mac personal
ssh mac 'bash ~/src/sunshine-personal/personal-mac/dev-build.sh --test "WebUICert*"'
```

`--test` takes a GoogleTest filter and runs only those tests; `--no-install` only builds. The first build installs
the Homebrew dependencies and compiles everything; later ones rebuild what changed.

`personal-mac/build-sunshine-mac.sh` is the older one-shot build. It signs ad hoc, so macOS asks for the
permissions again after every rebuild.
