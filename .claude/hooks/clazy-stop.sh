#!/usr/bin/env bash
# Stop hook: batch-run clazy-standalone on modified/untracked C++ sources in
# tools/crawler-webengine/ via git diff. Blocks the stop if any clazy
# diagnostics surface so Claude must address them before ending the turn.

set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="$repo_root/tools/crawler-webengine/cmake-build"
[ -f "$build_dir/compile_commands.json" ] || exit 0

cd "$repo_root" || exit 0

# macOS 自带 /bin/bash 3.2 没有 mapfile，用 while read 兜底；显式初始化空数组
# 避免 set -u 在 hook 解析到老 bash 时把 `files` 当作 unbound 变量。
files=()
while IFS= read -r line; do
  [ -n "$line" ] && files+=("$line")
done < <(
  {
    git diff --name-only
    git diff --cached --name-only
    git ls-files --others --exclude-standard
  } 2>/dev/null | sort -u | grep -E '^tools/crawler-webengine/.*\.(cpp|cc|cxx)$' || true
)

[ "${#files[@]}" -eq 0 ] && exit 0

all_output=""
for f in "${files[@]}"; do
  [ -f "$f" ] || continue
  out="$(clazy-standalone --only-qt -p "$build_dir" "$f" 2>&1)"
  if printf '%s' "$out" | grep -qE '(warning|error):'; then
    all_output+=$'\n=== '"$f"$' ===\n'"$out"
  fi
done

if [ -n "$all_output" ]; then
  ctx=$'clazy found Qt-specific issues in modified C++ files under tools/crawler-webengine/. Resolve them before stopping:\n'"$all_output"
  jq -n --arg ctx "$ctx" '{
    decision: "block",
    reason: $ctx
  }'
fi

exit 0
