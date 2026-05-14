#!/usr/bin/env bash
# run_test.sh — orchestrate KVoF cross-machine TCP transfer test.
#
# Usage:
#   # Both binaries on same machine (loopback):
#   ./run_test.sh --local
#
#   # Server on remote machine (SSH key auth required):
#   ./run_test.sh --server-host <HOST> [--server-user <USER>]
#                 [--server-bin <path/to/kvof_server>]
#                 [--client-bin <path/to/kvof_client>]
#                 [--port <PORT>] [--slot-size <BYTES>] [--slots <N>]
#
# The script:
#   1. Starts kvof_server (locally or via SSH) and waits for "READY".
#   2. Runs kvof_client against it.
#   3. Reports overall PASS/FAIL and cleans up the server process.

set -euo pipefail

# ── Defaults ──────────────────────────────────────────────────────────────────

MODE="remote"           # "local" or "remote"
SERVER_HOST=""
SERVER_USER="${USER}"
SERVER_PORT=20000
SLOT_SIZE=4096
SLOTS=8
LOCAL_PORT=20100        # client-side ctrl port
READY_TIMEOUT=15        # seconds to wait for server READY line

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../build"

SERVER_BIN="${BUILD_DIR}/test/kvof_server"
CLIENT_BIN="${BUILD_DIR}/test/kvof_client"

# ── Argument parsing ──────────────────────────────────────────────────────────

while [[ $# -gt 0 ]]; do
    case "$1" in
        --local)           MODE="local"; shift ;;
        --server-host)     SERVER_HOST="$2"; shift 2 ;;
        --server-user)     SERVER_USER="$2"; shift 2 ;;
        --server-bin)      SERVER_BIN="$2"; shift 2 ;;
        --client-bin)      CLIENT_BIN="$2"; shift 2 ;;
        --port)            SERVER_PORT="$2"; shift 2 ;;
        --local-port)      LOCAL_PORT="$2"; shift 2 ;;
        --slot-size)       SLOT_SIZE="$2"; shift 2 ;;
        --slots)           SLOTS="$2"; shift 2 ;;
        --help)
            sed -n '/^# Usage/,/^$/p' "$0"
            exit 0
            ;;
        *) echo "Unknown argument: $1"; exit 1 ;;
    esac
done

if [[ "$MODE" == "remote" && -z "$SERVER_HOST" ]]; then
    echo "ERROR: --server-host is required for remote mode (or use --local)"
    exit 1
fi

# ── Helpers ───────────────────────────────────────────────────────────────────

SERVER_PID=""
SSH_PID=""

cleanup() {
    if [[ -n "$SERVER_PID" ]]; then
        kill "$SERVER_PID" 2>/dev/null || true
    fi
    if [[ -n "$SSH_PID" ]]; then
        kill "$SSH_PID" 2>/dev/null || true
    fi
    # Remote: send SIGTERM via SSH
    if [[ "$MODE" == "remote" && -n "$SERVER_HOST" ]]; then
        ssh "${SERVER_USER}@${SERVER_HOST}" \
            "pkill -f kvof_server || true" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

# Wait for the READY line from a process, with timeout.
wait_ready() {
    local pipe="$1"
    local timeout="$2"
    local deadline=$(( $(date +%s) + timeout ))

    while IFS= read -r line <&3; do
        echo "  [server] $line"
        if [[ "$line" == READY* ]]; then
            return 0
        fi
        if (( $(date +%s) > deadline )); then
            break
        fi
    done 3<"$pipe"
    return 1
}

# ── Start server ──────────────────────────────────────────────────────────────

SERVER_ARGS="--ip 0.0.0.0 --port ${SERVER_PORT} --slots ${SLOTS} --slot-size ${SLOT_SIZE}"
READY_PIPE=$(mktemp -u /tmp/kvof_ready.XXXXXX)
mkfifo "$READY_PIPE"
trap 'rm -f "$READY_PIPE"; cleanup' EXIT INT TERM

if [[ "$MODE" == "local" ]]; then
    echo "Starting server locally on port ${SERVER_PORT}..."
    CLIENT_SERVER_IP="127.0.0.1"

    "${SERVER_BIN}" ${SERVER_ARGS} >"$READY_PIPE" 2>/dev/null &
    SERVER_PID=$!

else
    echo "Starting server on ${SERVER_USER}@${SERVER_HOST} port ${SERVER_PORT}..."
    CLIENT_SERVER_IP="${SERVER_HOST}"

    # Copy binary to remote if a remote path was not explicitly given.
    REMOTE_BIN="/tmp/kvof_server"
    scp -q "${SERVER_BIN}" "${SERVER_USER}@${SERVER_HOST}:${REMOTE_BIN}"
    ssh -o BatchMode=yes "${SERVER_USER}@${SERVER_HOST}" \
        "chmod +x ${REMOTE_BIN} && ${REMOTE_BIN} ${SERVER_ARGS}" \
        >"$READY_PIPE" 2>/dev/null &
    SSH_PID=$!
fi

echo "Waiting for server READY (timeout ${READY_TIMEOUT}s)..."
if ! wait_ready "$READY_PIPE" "$READY_TIMEOUT"; then
    echo "ERROR: server did not print READY within ${READY_TIMEOUT}s"
    exit 1
fi
echo "Server is ready."

# ── Run client ────────────────────────────────────────────────────────────────

echo ""
echo "Running client tests → server ${CLIENT_SERVER_IP}:${SERVER_PORT}"
echo "─────────────────────────────────────────────────────────────────"

"${CLIENT_BIN}" \
    --server-ip   "${CLIENT_SERVER_IP}" \
    --server-port "${SERVER_PORT}" \
    --local-port  "${LOCAL_PORT}" \
    --slot-size   "${SLOT_SIZE}"

CLIENT_RC=$?

echo "─────────────────────────────────────────────────────────────────"
if [[ $CLIENT_RC -eq 0 ]]; then
    echo "Overall: PASS"
else
    echo "Overall: FAIL (client exit code ${CLIENT_RC})"
fi

exit $CLIENT_RC
