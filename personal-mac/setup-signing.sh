#!/usr/bin/env bash
# One-time: put the Developer ID signing key in a keychain of its own, so dev-build.sh can sign
# builds over SSH. Builds signed with the same identity as the releases keep their macOS permissions.
#
# Usage (from Windows):  ssh -t mac 'bash ~/src/sunshine-personal/personal-mac/setup-signing.sh'
# Paste CERT_KEY_B64 from your password manager when asked; it is not shown or saved.
set -euo pipefail
umask 077

KEYCHAIN="$HOME/Library/Keychains/sunshine-signing.keychain-db"
PASSWORD_FILE="$HOME/.config/sunshine-build/keychain-password"
APP=/Applications/Sunshine.app

die() { echo "error: $*" >&2; exit 1; }

[[ -d "$APP" ]] || die "install a signed Sunshine release first; its certificate is copied from $APP"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# The certificates are public, so take them from the installed release instead of the developer portal.
(cd "$tmp" && codesign -d --extract-certificates "$APP" 2>/dev/null)
[[ -f "$tmp/codesign0" ]] || die "$APP is not signed with a certificate"
openssl x509 -inform DER -in "$tmp/codesign0" -out "$tmp/cert.pem"

read -rsp "Paste CERT_KEY_B64, then press Enter: " key_b64
echo
printf '%s' "$key_b64" | openssl base64 -d -A > "$tmp/key.pem"
unset key_b64

openssl x509 -in "$tmp/cert.pem" -noout -pubkey > "$tmp/cert.pub"
openssl pkey -in "$tmp/key.pem" -pubout > "$tmp/key.pub" 2>/dev/null || die "that is not a valid key"
cmp -s "$tmp/cert.pub" "$tmp/key.pub" || die "that key does not belong to the certificate Sunshine is signed with"

mkdir -p "$(dirname "$PASSWORD_FILE")"
[[ -f "$PASSWORD_FILE" ]] || openssl rand -hex 24 > "$PASSWORD_FILE"
password="$(cat "$PASSWORD_FILE")"

[[ -f "$KEYCHAIN" ]] || security create-keychain -p "$password" "$KEYCHAIN"
# No auto-lock, so a long build can still sign at the end.
security set-keychain-settings "$KEYCHAIN"
security unlock-keychain -p "$password" "$KEYCHAIN"

# /usr/bin/openssl is LibreSSL, whose PKCS#12 encryption `security import` can read.
p12_password="$(openssl rand -hex 16)"
/usr/bin/openssl pkcs12 -export -inkey "$tmp/key.pem" -in "$tmp/cert.pem" -out "$tmp/identity.p12" \
  -passout "pass:$p12_password"
security import "$tmp/identity.p12" -k "$KEYCHAIN" -P "$p12_password" -T /usr/bin/codesign >/dev/null
for intermediate in "$tmp"/codesign[1-9]; do
  security import "$intermediate" -k "$KEYCHAIN" >/dev/null 2>&1 || true
done
# Lets codesign use the key without a GUI prompt, which an SSH session can't answer.
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$password" "$KEYCHAIN" >/dev/null

# codesign only finds identities in keychains on the search list.
search_list=()
while IFS= read -r line; do
  line="${line#"${line%%[![:space:]]*}"}"
  search_list+=("${line//\"/}")
done < <(security list-keychains -d user)
if [[ ! " ${search_list[*]} " == *" $KEYCHAIN "* ]]; then
  security list-keychains -d user -s "${search_list[@]}" "$KEYCHAIN"
fi

security find-identity -v -p codesigning "$KEYCHAIN" | grep -q '"Developer ID Application' ||
  die "the identity was imported but codesign can't see it"
echo "Done: builds on this Mac are now signed like the releases."
