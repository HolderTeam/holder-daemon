#!/usr/bin/env bash
set -euo pipefail

DATA_DIR_DEFAULT="${HOME}/.local/share/holder"
CONFIG_DIR_DEFAULT="${HOME}/.config/holder"
CACHE_DIR_DEFAULT="${HOME}/.cache/holder"

DATA_DIR="${DATA_DIR_DEFAULT}"
CONFIG_DIR="${CONFIG_DIR_DEFAULT}"
CACHE_DIR="${CACHE_DIR_DEFAULT}"
FORCE=0

usage() {
  echo "Usage: $0 [--data-dir <path>] [--config-dir <path>] [--cache-dir <path>] --force"
  echo "Deletes holder data, config, and cache directories."
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --data-dir)
      DATA_DIR="$2"
      shift 2
      ;;
    --config-dir)
      CONFIG_DIR="$2"
      shift 2
      ;;
    --cache-dir)
      CACHE_DIR="$2"
      shift 2
      ;;
    --force|--yes)
      FORCE=1
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

if [[ $FORCE -ne 1 ]]; then
  echo "Refusing to run without --force"
  usage
  exit 2
fi

LOCK_PATH="${DATA_DIR}/server/holder.lock"
if [[ -f "$LOCK_PATH" ]]; then
  echo "Lock file exists at $LOCK_PATH. Stop holder before running factory reset."
  exit 3
fi

rm -rf "$DATA_DIR" "$CONFIG_DIR" "$CACHE_DIR"

echo "Factory reset complete:"
 echo "- data:   $DATA_DIR"
 echo "- config: $CONFIG_DIR"
 echo "- cache:  $CACHE_DIR"
