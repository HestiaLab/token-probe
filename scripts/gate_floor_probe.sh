#!/usr/bin/env bash
# 门禁下限探测：扫描面为空时，每个 scripts/check_*.py 质量门禁都必须失败
# （非零退出），并在第一行输出命名诊断。空扫描却打印 OK 的门禁属于缺陷。
#
# 用法：./scripts/gate_floor_probe.sh
# 退出码：0 = 所有被探测门禁都按要求失败；1 = 至少有一个门禁空泛通过或
#         探测结果不确定。
#
# 说明：
# - 探测目标通过通配符发现（scripts/check_*.py），新门禁自动纳入。
# - 需要额外必需参数的门禁在下方有单独的 case 条目；未知门禁按
#   "<空目录>" 约定探测，任何用法错误退出（2）都报告为 INCONCLUSIVE，
#   而不是通过。
set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

PYTHON=${PYTHON:-python3}
EMPTY_DIR=$(mktemp -d "${TMPDIR:-/tmp}/gate-floor-probe-XXXXXX")
trap 'rm -rf "$EMPTY_DIR"' EXIT

overall=0
probed=0

printf '%-34s %-6s %s\n' "GATE" "EXIT" "FIRST LINE"
for gate in "$REPO_ROOT"/scripts/check_*.py; do
  [ -f "$gate" ] || continue
  name=$(basename "$gate")
  case "$name" in
    check_version_consistency.py)
      out=$("$PYTHON" "$gate" "$EMPTY_DIR" \
        --build-dir "$EMPTY_DIR" --exe "$EMPTY_DIR/no-such-exe" 2>&1)
      rc=$?
      ;;
    *)
      out=$("$PYTHON" "$gate" "$EMPTY_DIR" 2>&1)
      rc=$?
      ;;
  esac
  first_line=$(printf '%s\n' "$out" | head -n 1)
  probed=$((probed + 1))

  verdict="FAIL-AS-REQUIRED"
  if [ "$rc" -eq 0 ]; then
    verdict="VACUOUS-PASS"
    overall=1
  elif [ "$rc" -eq 2 ]; then
    verdict="INCONCLUSIVE (usage error; add a probe case for this gate)"
    overall=1
  elif ! printf '%s' "$first_line" | grep -qE 'FAIL|ERROR|解析失败'; then
    verdict="NO-NAMED-DIAGNOSTIC"
    overall=1
  fi

  printf '%-34s %-6s %s\n' "$name" "$rc" "$first_line"
  printf '%-34s %-6s -> %s\n' "" "" "$verdict"
done

if [ "$probed" -eq 0 ]; then
  echo "FAIL: no scripts/check_*.py gates found to probe" >&2
  exit 1
fi

echo ""
if [ "$overall" -eq 0 ]; then
  echo "OK: all $probed probed gates fail on an empty scan surface"
else
  echo "FAIL: at least one gate does not fail properly on an empty scan surface" >&2
fi
exit "$overall"
