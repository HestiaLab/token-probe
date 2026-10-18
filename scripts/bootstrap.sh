#!/usr/bin/env sh
set -eu

PRESET="ninja-debug"
CHECK_ONLY=0
CHECK_EDITOR=0
FAILED=0
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

cd "$REPO_ROOT"

show_usage() {
  echo "Usage: $0 [--preset <name>] [--check-only]"
  echo "       $0 [preset] [--check-only]"
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
    --check-only)
      CHECK_ONLY=1
      shift
      ;;
    --check-editor)
      CHECK_EDITOR=1
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

check_tool() {
  name="$1"
  hint="$2"
  required="$3"

  if command -v "$name" >/dev/null 2>&1; then
    echo "OK: found $name"
    return 0
  fi

  if [ "$required" = "required" ]; then
    echo "ERROR: missing $name. $hint"
    FAILED=1
  else
    echo "Optional: missing $name. $hint"
  fi
  return 0
}

check_compiler() {
  if [ -n "${CC:-}" ]; then
    echo "OK: using compiler from CC=$CC"
    CC_PATH=$(command -v "$CC" 2>/dev/null || true)
    return 0
  fi

  for compiler in cc gcc clang cl; do
    CC_PATH=$(command -v "$compiler" 2>/dev/null || true)
    if [ -n "${CC_PATH:-}" ]; then
      echo "OK: found C compiler command $compiler"
      return 0
    fi
  done

  case "$(uname -s 2>/dev/null || echo unknown)" in
    MINGW*|MSYS*|CYGWIN*)
      echo "ERROR: no C compiler command found. Open Developer PowerShell, Developer Command Prompt, or select an MSVC CMake kit in VS Code."
      ;;
    *)
      echo "ERROR: no C compiler command found. Install GCC or Clang (including AppleClang via Xcode Command Line Tools), or set CC to your C compiler."
      ;;
  esac
  FAILED=1
}

check_cmake_version() {
  version_line=$(cmake --version 2>/dev/null | sed -n '1p')
  version=$(printf '%s\n' "$version_line" | sed -n 's/.*version \([0-9][0-9]*\)\.\([0-9][0-9]*\)\.\([0-9][0-9]*\).*/\1 \2 \3/p')
  if [ -z "$version" ]; then
    echo "ERROR: could not determine CMake version."
    FAILED=1
    return 0
  fi

  set -- $version
  major="$1"
  minor="$2"
  patch="$3"
  if [ "$major" -gt 3 ] || { [ "$major" -eq 3 ] && [ "$minor" -ge 21 ]; }; then
    echo "OK: CMake version $major.$minor.$patch"
  else
    echo "ERROR: CMake $major.$minor.$patch is too old. Install CMake 3.21 or newer."
    FAILED=1
  fi
}

check_tool cmake "Install CMake 3.21 or newer and add it to PATH." required
if command -v cmake >/dev/null 2>&1; then
  check_cmake_version
fi

case "$PRESET" in
  ninja-*) check_tool ninja "Install Ninja or choose a generator-neutral preset through CMakeUserPresets.json." required ;;
esac

check_compiler

# 确保编译器目录在 PATH 中，使 CMake 子进程能找到运行时 DLL
# （适用于 MinGW、MSYS2 和独立安装的 LLVM）。
if [ -n "${CC_PATH:-}" ]; then
  compiler_dir=$(dirname "$CC_PATH")
  export PATH="$compiler_dir:$PATH"
  echo "OK: added $compiler_dir to PATH for compiler runtime"
fi

check_tool clangd "Install clangd for editor navigation." optional
check_tool clang-format "Install clang-format for local format checks." optional
check_tool clang-tidy "Install clang-tidy for optional static analysis." optional

if [ "$CHECK_EDITOR" -eq 1 ]; then
  check_tool gdb "Install gdb for debugging (MSYS2: pacman -S mingw-w64-ucrt-x86_64-gdb)." required
fi

if [ "$FAILED" -ne 0 ]; then
  exit 1
fi

if [ "$CHECK_ONLY" -eq 0 ]; then
  cmake --preset "$PRESET"

  compile_commands="build/$PRESET/compile_commands.json"
  if [ -f "$compile_commands" ]; then
    echo "OK: generated $compile_commands"
  else
    echo "Note: $compile_commands was not generated. Use a generator that supports compilation databases for clangd."
  fi
fi
