#!/usr/bin/env sh
set -eu

PRESET="ninja-debug"
ENABLE_TIDY=0
SKIP_FORMAT=0
FIX_FORMAT=0
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

cd "$REPO_ROOT"

show_usage() {
  echo "Usage: $0 [--preset <name>] [--skip-format] [--enable-tidy]"
  echo "       $0 [preset] [tidy|--tidy]"
}

PRESET_SET=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --preset)
      if [ "$#" -lt 2 ]; then
        echo "ERROR: --preset requires a value." >&2
        exit 2
      fi
      PRESET="$2"
      PRESET_SET=1
      shift 2
      ;;
    --skip-format)
      SKIP_FORMAT=1
      shift
      ;;
    --fix-format)
      FIX_FORMAT=1
      shift
      ;;
    --enable-tidy|--tidy|tidy)
      ENABLE_TIDY=1
      shift
      ;;
    -h|--help)
      show_usage
      exit 0
      ;;
    -*)
      echo "ERROR: unknown option $1" >&2
      show_usage >&2
      exit 2
      ;;
    *)
      if [ "$PRESET_SET" -ne 0 ]; then
        echo "ERROR: unexpected argument $1" >&2
        show_usage >&2
        exit 2
      fi
      PRESET="$1"
      PRESET_SET=1
      shift
      ;;
  esac
done

if [ "$SKIP_FORMAT" -eq 0 ]; then
  if [ "$FIX_FORMAT" -ne 0 ]; then
    sh "$SCRIPT_DIR/static_analysis.sh" format-check --fix
  else
    sh "$SCRIPT_DIR/static_analysis.sh" format-check
  fi
fi

cmake --preset "$PRESET"
cmake --build --preset "$PRESET"

if [ "$ENABLE_TIDY" -ne 0 ]; then
  sh "$SCRIPT_DIR/static_analysis.sh" tidy --build-dir "build/$PRESET"
fi

ctest --preset "$PRESET" --output-on-failure
