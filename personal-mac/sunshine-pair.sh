#!/usr/bin/env bash
# Approve a Moonlight pairing request from a terminal on the Mac, e.g. over SSH with no screen.
# Start pairing in Moonlight first and leave its PIN dialog open.
#
# Usage:  bash sunshine-pair.sh [PIN] [device name]
#         bash sunshine-pair.sh --cancel     drop every waiting request (fixes Moonlight's error 409)
# Env:    SUNSHINE_USER=<web UI username>    (asked for when unset)
#         SUNSHINE_URL=https://localhost:47990
set -euo pipefail

URL="${SUNSHINE_URL:-https://localhost:47990}"

die() { echo "error: $*" >&2; exit 1; }

command -v jq >/dev/null || die "jq is missing (it ships with macOS 15 and newer)"

user="${SUNSHINE_USER:-}"
[[ -n "$user" ]] || read -rp "Sunshine web UI username: " user
read -rsp "Sunshine web UI password: " password
echo

# A private curl config file keeps the password out of ps and the shell history.
auth="$(mktemp)"
trap 'rm -f "$auth"' EXIT
chmod 600 "$auth"
credentials="${user}:${password}"
credentials="${credentials//\\/\\\\}"
printf 'user = "%s"\n' "${credentials//\"/\\\"}" > "$auth"
unset password credentials

api() {
  local method=$1 body=${2:-} out code
  local args=(-sS -k -K "$auth" -X "$method" -w $'\n%{http_code}')
  [[ -z "$body" ]] || args+=(-H 'Content-Type: application/json' --data "$body")
  out="$(curl "${args[@]}" "$URL/api/pin")" || die "can't reach Sunshine at $URL. Is it running?"
  code="${out##*$'\n'}"
  out="${out%$'\n'*}"
  case "$code" in
    200) printf '%s' "$out" ;;
    401) die "wrong username or password" ;;
    3*) die "Sunshine has no web UI login yet. Create one in the web UI first." ;;
    *) die "Sunshine answered $code: $out" ;;
  esac
}

pending="$(api GET | jq -c '.pairings')"
count="$(jq length <<<"$pending")"

if [[ "${1:-}" == "--cancel" ]]; then
  for id in $(jq -r '.[].id' <<<"$pending"); do
    api DELETE "$(jq -cn --arg id "$id" '{pairing_id: $id}')" >/dev/null
  done
  echo "Cancelled $count waiting request(s). Pair again from Moonlight."
  exit 0
fi

(( count > 0 )) || die "no device is waiting. In Moonlight, start pairing with the Mac and run this while the PIN shows."

index=0
if (( count > 1 )); then
  jq -r 'to_entries[] | "\(.key + 1)) \(.value.name) from \(.value.address)"' <<<"$pending"
  read -rp "Which one? " choice
  if [[ ! "$choice" =~ ^[0-9]+$ ]] || (( choice < 1 || choice > count )); then
    die "pick a number from 1 to $count"
  fi
  index=$((choice - 1))
fi
id="$(jq -r ".[$index].id" <<<"$pending")"
requested_name="$(jq -r ".[$index].name" <<<"$pending")"
echo "Waiting: ${requested_name} from $(jq -r ".[$index].address" <<<"$pending")"

pin="${1:-}"
[[ -n "$pin" ]] || read -rp "PIN shown in Moonlight: " pin
[[ "$pin" =~ ^[0-9]{4}$ ]] || die "the PIN is 4 digits"

name="${2:-}"
[[ -n "$name" ]] || read -rp "Name for this device [${requested_name}]: " name
name="${name:-$requested_name}"

result="$(api POST "$(jq -cn --arg id "$id" --arg pin "$pin" --arg name "$name" '{pairing_id: $id, pin: $pin, name: $name}')")"
if [[ "$(jq -r '.status' <<<"$result")" == "true" ]]; then
  echo "Paired ${name}."
else
  die "pairing failed: wrong PIN, or Moonlight's PIN dialog was closed. Start pairing again."
fi
