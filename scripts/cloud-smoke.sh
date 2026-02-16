#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  cloud-smoke.sh --provider <id> [--model <model-id>] [--prompt <text>] [options]

Required:
  --provider <id>          Cloud provider id (e.g. switchyard, chadjeopardy, mechatropic)

Auth (one required):
  --api-key <key>          API key for provider
  HOLDER_CLOUD_API_KEY     Env fallback for API key

Server auth (one required):
  --token <token>          Holder auth token
  HOLDER_TOKEN             Env fallback for token

Optional:
  --host <host>            Default: 127.0.0.1
  --port <port>            Default: 11499
  --project-id <id>        Optional. If set, run is attached to that project.
  --prompt <text>          Default: "Say 'cloud smoke ok' and nothing else."
  --timeout-seconds <n>    Max seconds for /ai/runs stream. Default: 120
  --skip-credential-upsert Do not call PUT /ai/providers/credentials first
  -h, --help               Show this help
EOF
}

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing dependency: $1"
    exit 2
  fi
}

HOST="127.0.0.1"
PORT="11499"
PROVIDER=""
MODEL=""
PROJECT_ID=""
PROMPT="Say 'cloud smoke ok' and nothing else."
API_KEY="${HOLDER_CLOUD_API_KEY:-}"
TOKEN="${HOLDER_TOKEN:-}"
SKIP_CREDENTIAL_UPSERT=0
TIMEOUT_SECONDS="120"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host)
      HOST="$2"
      shift 2
      ;;
    --port)
      PORT="$2"
      shift 2
      ;;
    --provider)
      PROVIDER="$2"
      shift 2
      ;;
    --model)
      MODEL="$2"
      shift 2
      ;;
    --project-id)
      PROJECT_ID="$2"
      shift 2
      ;;
    --prompt)
      PROMPT="$2"
      shift 2
      ;;
    --timeout-seconds)
      TIMEOUT_SECONDS="$2"
      shift 2
      ;;
    --api-key)
      API_KEY="$2"
      shift 2
      ;;
    --token)
      TOKEN="$2"
      shift 2
      ;;
    --skip-credential-upsert)
      SKIP_CREDENTIAL_UPSERT=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1"
      usage
      exit 2
      ;;
  esac
done

require_cmd curl
require_cmd jq

if [[ -z "$PROVIDER" ]]; then
  echo "--provider is required"
  usage
  exit 2
fi
if [[ -z "$TOKEN" ]]; then
  echo "Missing token. Use --token or HOLDER_TOKEN."
  exit 2
fi
if [[ $SKIP_CREDENTIAL_UPSERT -eq 0 && -z "$API_KEY" ]]; then
  echo "Missing API key. Use --api-key or HOLDER_CLOUD_API_KEY."
  exit 2
fi

BASE_URL="http://${HOST}:${PORT}"
AUTH_HEADER="Authorization: Bearer ${TOKEN}"

echo "Cloud smoke target: ${BASE_URL} provider=${PROVIDER}"

if [[ $SKIP_CREDENTIAL_UPSERT -eq 0 ]]; then
  cred_payload="$(jq -cn --arg provider "$PROVIDER" --arg api_key "$API_KEY" \
    '{provider:$provider, api_key:$api_key}')"
  cred_resp="$(curl -fsS "${BASE_URL}/ai/providers/credentials" \
    -X PUT \
    -H "$AUTH_HEADER" \
    -H "Content-Type: application/json" \
    -d "$cred_payload")"

  if ! jq -e '.ok == true' >/dev/null <<<"$cred_resp"; then
    echo "Credential upsert failed:"
    echo "$cred_resp"
    exit 1
  fi
  echo "Credential upsert ok."
fi

run_payload="$(jq -cn \
  --arg prompt "$PROMPT" \
  --arg provider "$PROVIDER" \
  --arg project_id "$PROJECT_ID" \
  '
  {
    prompt: $prompt,
    provider: $provider
  }
  + (if $project_id == "" then {} else {project_id: $project_id} end)
  ')"

echo "Starting /ai/runs ..."
tmp_sse_file="$(mktemp)"
trap 'rm -f "$tmp_sse_file"' EXIT
run_status="$(curl -sS -N --max-time "$TIMEOUT_SECONDS" "${BASE_URL}/ai/runs" \
  -X POST \
  -H "$AUTH_HEADER" \
  -H "Content-Type: application/json" \
  -d "$run_payload" \
  -o "$tmp_sse_file" \
  -w "%{http_code}")"
if [[ "$run_status" -lt 200 || "$run_status" -ge 300 ]]; then
  echo "POST /ai/runs failed with HTTP ${run_status}:"
  cat "$tmp_sse_file"
  exit 1
fi
sse_resp="$(cat "$tmp_sse_file")"
if [[ -z "$sse_resp" ]]; then
  echo "Empty SSE response body from /ai/runs."
  exit 1
fi

run_id="$(printf '%s' "$sse_resp" | grep -o '"run_id":"[^"]*"' | head -n1 | cut -d'"' -f4 || true)"
if [[ -z "$run_id" ]]; then
  echo "Could not extract run_id from SSE response:"
  echo "$sse_resp"
  echo "Recent runs snapshot:"
  curl -sS "${BASE_URL}/ai/runs?project_id=${PROJECT_ID}" -H "$AUTH_HEADER" || true
  exit 1
fi

echo "Run id: $run_id"
if grep -q '^event: failed$' <<<"$sse_resp"; then
  echo "Run stream ended with failed event."
fi

run_resp="$(curl -fsS "${BASE_URL}/ai/runs/${run_id}" -H "$AUTH_HEADER")"
if ! jq -e '.ok == true' >/dev/null <<<"$run_resp"; then
  echo "Failed to load run record:"
  echo "$run_resp"
  exit 1
fi

status="$(jq -r '.data.status // empty' <<<"$run_resp")"
chosen_model="$(jq -r '.data.chosen_model // empty' <<<"$run_resp")"
trace_provider="$(jq -r '.data.policy_trace.provider // empty' <<<"$run_resp")"
trace_result="$(jq -r '.data.policy_trace.result.status // empty' <<<"$run_resp")"
error_text="$(jq -r '.data.error // empty' <<<"$run_resp")"

echo "Run status: ${status}"
echo "Chosen model: ${chosen_model}"
echo "Trace provider: ${trace_provider}"
echo "Trace result: ${trace_result}"

if [[ "$trace_provider" != "$PROVIDER" ]]; then
  echo "Provider mismatch: expected '${PROVIDER}', got '${trace_provider}'"
  exit 1
fi

if [[ "$status" != "completed" || "$trace_result" != "completed" ]]; then
  echo "Cloud smoke failed. Error: ${error_text}"
  exit 1
fi

if [[ -n "$MODEL" ]]; then
  if [[ "$chosen_model" != "${PROVIDER}:"* ]]; then
    echo "Chosen model prefix mismatch: expected provider '${PROVIDER}', got '${chosen_model}'"
    exit 1
  fi
fi

echo "Cloud smoke passed."
