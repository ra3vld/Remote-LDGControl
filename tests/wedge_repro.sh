#!/usr/bin/env bash
# Read-only runtime web check: the server must respond after SSE clients close.
# Set SCHEME=https when testing a TLS_ENABLED=1 build.
#
# Args:
#   $1 = device IP (default 192.168.10.188)
#   $2 = auth user:pass (default testing:testing)
#   $3 = number of SSE subscribers to open (default 2)
#   $4 = seconds each subscriber stays open before kill (default 4)
#   $5 = seconds to allow connection cleanup after kill (default 2)
set -u
IP="${1:-192.168.10.188}"
AUTH="${2:-testing:testing}"
N="${3:-2}"
HOLD="${4:-4}"
SAMPLE="${5:-2}"
SCHEME="${SCHEME:-http}"
curl_flags=(-s --noproxy '*')
case "$SCHEME" in
    http) ;;
    https) curl_flags+=(-k) ;;
    *) echo "SCHEME must be http or https"; exit 2 ;;
esac

echo "=== wedge_repro: $N SSE subs, hold ${HOLD}s, cleanup ${SAMPLE}s ==="

# Spawn N curl SSE subscribers in background.
pids=()
for ((i=0; i<N; i++)); do
    (curl "${curl_flags[@]}" -u "$AUTH" -N --max-time $((HOLD + 60)) \
          -H 'Accept: text/event-stream' \
          "$SCHEME://$IP/api/events" >/dev/null 2>&1) &
    pids+=("$!")
done
echo "  spawned ${#pids[@]} SSE subscribers: ${pids[*]}"

# Let them subscribe.
sleep "$HOLD"

# Kill them all roughly simultaneously (TERM, fall back to KILL).
echo "  killing all subscribers..."
for pid in "${pids[@]}"; do kill -TERM "$pid" 2>/dev/null; done
sleep 0.5
for pid in "${pids[@]}"; do kill -KILL "$pid" 2>/dev/null; done
wait 2>/dev/null

# Allow the HTTP task to release the closed connection slots.
sleep "$SAMPLE"

# Probe whether the server still responds.
post_status=$(curl "${curl_flags[@]}" -u "$AUTH" --max-time 5 -o /dev/null \
                  -w "%{http_code}" "$SCHEME://$IP/api/config" 2>/dev/null)

echo "--- RESULT ---"
echo "  GET /api/config response: HTTP $post_status"
if [ "$post_status" = "200" ] || [ "$post_status" = "401" ]; then
    echo "  VERDICT: HTTP server still responsive after SSE disconnects"
    exit 0
else
    echo "  VERDICT: HTTP server unresponsive (HTTP $post_status)"
    exit 2
fi
