#!/usr/bin/env bash
# 静态分析入口——CI（.github/workflows/static-analysis.yml）和本地
# scripts/check.sh 共用的唯一实现。
#
# 子命令：
#   format-check   对 include/、src/、tests/ 运行
#                  clang-format --dry-run --Werror（排除 tests/vendor）；
#                  --fix 则就地重写文件。
#   tidy           对 src/ 和 tests/ 运行 clang-tidy --warnings-as-errors='*'
#                  （排除 package_smoke、subproject_smoke、vendor），
#                  有编译数据库时优先使用。
#   cppcheck       对 src/ 和 tests/ 运行 cppcheck --error-exitcode=1
#                  （排除 tests/vendor）。
#
# 选项：
#   --build-dir <dir>   编译数据库位置（默认 build/ninja-debug）
#   --require-tools     工具缺失即硬失败（CI 语义）。不传时工具缺失只打印
#                       跳过说明并以 0 退出（本地便利语义，与 check.sh 一致）。
#
# 只有 *.c/*.h/*.cpp 会传给 clang-format，绝不包含 .py 或其他文件类型。
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
cd "$REPO_ROOT"

FILE_LIST=".static-analysis-files.txt"
trap 'rm -f "$FILE_LIST"' EXIT

BUILD_DIR="build/ninja-debug"
REQUIRE_TOOLS=0
FIX=0
MODE=""

show_usage() {
  echo "Usage: $0 {format-check|tidy|cppcheck} [--build-dir <dir>] [--require-tools] [--fix]"
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    format-check|tidy|cppcheck)
      if [ -n "$MODE" ]; then echo "ERROR: only one subcommand allowed" >&2; exit 2; fi
      MODE="$1"; shift ;;
    --build-dir)
      [ "$#" -ge 2 ] || { echo "ERROR: --build-dir requires a value" >&2; exit 2; }
      BUILD_DIR="$2"; shift 2 ;;
    --require-tools) REQUIRE_TOOLS=1; shift ;;
    --fix) FIX=1; shift ;;
    -h|--help) show_usage; exit 0 ;;
    *) echo "ERROR: unknown argument: $1" >&2; show_usage >&2; exit 2 ;;
  esac
done

if [ -z "$MODE" ]; then show_usage >&2; exit 2; fi

tool_missing() {
  # $1 = 工具名称
  if [ "$REQUIRE_TOOLS" -ne 0 ]; then
    echo "ERROR: required tool not found: $1" >&2
    exit 1
  fi
  echo "Skipping $1: command not found."
  exit 0
}

case "$MODE" in
  format-check)
    command -v clang-format >/dev/null 2>&1 || tool_missing clang-format
    # 文件清单：C/C++ 源文件和头文件，排除第三方代码。
    find include src tests \( -path tests/vendor \) -prune -o \
      -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' \) -print \
      | sort > .static-analysis-files.txt
    if [ ! -s .static-analysis-files.txt ]; then
      echo "ERROR: format scan surface is empty (no .c/.h/.cpp found)" >&2
      rm -f .static-analysis-files.txt
      exit 1
    fi
    if [ "$FIX" -ne 0 ]; then
      xargs clang-format -i < .static-analysis-files.txt
    else
      xargs clang-format --dry-run --Werror < .static-analysis-files.txt
    fi
    rm -f .static-analysis-files.txt
    ;;
  tidy)
    command -v clang-tidy >/dev/null 2>&1 || tool_missing clang-tidy
    find src tests \
      \( -path tests/package_smoke -o -path tests/subproject_smoke -o -path tests/vendor \) \
      -prune -o -type f -name '*.c' -print \
      | sort > .static-analysis-files.txt
    if [ ! -s .static-analysis-files.txt ]; then
      echo "ERROR: clang-tidy scan surface is empty (no .c found)" >&2
      rm -f .static-analysis-files.txt
      exit 1
    fi
    GENERATED_INCLUDE_DIR="$BUILD_DIR/generated/include"
    if [ -f "$BUILD_DIR/compile_commands.json" ]; then
      xargs clang-tidy --warnings-as-errors='*' -p "$BUILD_DIR" < .static-analysis-files.txt
    else
      # 没有编译数据库：逐文件运行，在 `--` 之后显式给出编译参数
      # （与旧版 check.sh 语义一致）。
      xargs -I{} clang-tidy --warnings-as-errors='*' --extra-arg=-std=c11 \
        {} -- -Iinclude -I"$GENERATED_INCLUDE_DIR" < .static-analysis-files.txt
    fi
    rm -f .static-analysis-files.txt
    ;;
  cppcheck)
    command -v cppcheck >/dev/null 2>&1 || tool_missing cppcheck
    find src tests \( -path tests/vendor \) -prune -o -type f -name '*.c' -print \
      | sort > .static-analysis-files.txt
    if [ ! -s .static-analysis-files.txt ]; then
      echo "ERROR: cppcheck scan surface is empty (no .c found)" >&2
      rm -f .static-analysis-files.txt
      exit 1
    fi
    cppcheck \
      --std=c11 \
      --enable=warning,performance,portability \
      --max-configs=1 \
      --quiet \
      --error-exitcode=1 \
      --file-list=.static-analysis-files.txt \
      -I include \
      -I "$BUILD_DIR/generated/include"
    rm -f .static-analysis-files.txt
    ;;
esac
