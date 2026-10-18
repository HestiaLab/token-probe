#!/usr/bin/env sh
# 就地重命名模板项目标识符。
#
# ** 推荐使用交互式设置向导： **
#     cmake --preset ninja-debug && cmake --build --preset ninja-debug
#     ./build/ninja-debug/bin/tokenprobe init --in-place
#
# 本脚本保留给 CI、自动化和非交互场景。
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
cd "$REPO_ROOT"

show_usage() {
  echo "Usage: $0 <PascalCaseName>"
  echo ""
  echo "Renames this template project in-place. Run once after cloning."
  echo ""
  echo "Example:"
  echo "  $0 MyNewLib"
  echo ""
  echo "The script derives the lowercase namespace (mylib) and UPPER_CASE"
  echo "macro prefix (MYLIB) from the PascalCase name automatically."
  echo ""
  echo "After renaming, run ./scripts/bootstrap.sh to verify the build."
  exit 0
}

if [ "$#" -ne 1 ]; then
  show_usage
fi

case "$1" in
  -h|--help) show_usage ;;
esac

PASCAL="$1"

# 校验：必须以大写字母开头，且只包含字母和数字
if ! echo "$PASCAL" | grep -qE '^[A-Z][A-Za-z0-9]*$'; then
  echo "ERROR: project name must start with an uppercase letter and contain only letters and digits." >&2
  echo "Example: MyNewLib" >&2
  exit 2
fi

LOWER=$(echo "$PASCAL" | tr '[:upper:]' '[:lower:]')
UPPER=$(echo "$LOWER" | tr '[:lower:]' '[:upper:]')

echo "Project rename:"
echo "  PascalCase: TokenProbe -> ${PASCAL}"
echo "  lowercase:  tokenprobe        -> ${LOWER}"
echo "  UPPER_CASE: TOKENPROBE        -> ${UPPER}"
echo ""

# 查找包含任一旧标识符的文件，排除 build/、.git/ 和脚本自身
files=$(find . -type f \
  \( -name '*.c' -o -name '*.h' -o -name '*.cmake' -o -name '*.txt' \
     -o -name '*.json' -o -name '*.yml' -o -name '*.md' -o -name '*.in' \
     -o -name '*.cpp' -o -name '*.ps1' -o -name '*.sh' \) \
  ! -path './build/*' ! -path './.git/*' \
  ! -path './scripts/rename-project.sh' \
  ! -path './scripts/rename-project.ps1' \
  -exec grep -l -e 'TokenProbe' -e 'tokenprobe' -e 'TOKENPROBE' {} + 2>/dev/null || true)

if [ -z "$files" ]; then
  echo "No files found with original project identifiers."
  echo "The project may already be renamed."
  exit 0
fi

updated=0
for file in $files; do
  count_pascal_std=$(grep -o 'TokenProbe' "$file" 2>/dev/null | wc -l | tr -d ' ')
  count_lower=$(grep -o 'tokenprobe' "$file" 2>/dev/null | wc -l | tr -d ' ')
  count_upper=$(grep -o 'TOKENPROBE' "$file" 2>/dev/null | wc -l | tr -d ' ')

  sed \
    -e "s/TokenProbe/${PASCAL}/g" \
    -e "s/tokenprobe/${LOWER}/g" \
    -e "s/TOKENPROBE/${UPPER}/g" \
    "$file" > "${file}.tmp" && mv "${file}.tmp" "$file"

  echo "  $file"
  if [ "$count_pascal_std" -gt 0 ]; then
    echo "    TokenProbe -> ${PASCAL} ($count_pascal_std)"
  fi
  if [ "$count_lower" -gt 0 ]; then
    echo "    tokenprobe -> ${LOWER} ($count_lower)"
  fi
  if [ "$count_upper" -gt 0 ]; then
    echo "    TOKENPROBE -> ${UPPER} ($count_upper)"
  fi
  updated=$((updated + 1))
done

echo ""
echo "Updated ${updated} files with new project identifiers."

# 重命名目录
if [ -d "include/tokenprobe" ]; then
  mv "include/tokenprobe" "include/${LOWER}"
  echo "Renamed directory: include/tokenprobe -> include/${LOWER}"
fi

if [ -d "src/tokenprobe" ]; then
  mv "src/tokenprobe" "src/${LOWER}"
  echo "Renamed directory: src/tokenprobe -> src/${LOWER}"
fi

# 重命名 cmake 文件
if [ -f "cmake/TokenProbeConfig.cmake.in" ]; then
  mv "cmake/TokenProbeConfig.cmake.in" "cmake/${PASCAL}Config.cmake.in"
  echo "Renamed file: TokenProbeConfig.cmake.in -> ${PASCAL}Config.cmake.in"
fi

if [ -f "cmake/tokenprobe_version.h.in" ]; then
  mv "cmake/tokenprobe_version.h.in" "cmake/${LOWER}_version.h.in"
  echo "Renamed file: tokenprobe_version.h.in -> ${LOWER}_version.h.in"
fi

echo ""
echo "Done. Run ./scripts/bootstrap.sh to verify the renamed project builds."
