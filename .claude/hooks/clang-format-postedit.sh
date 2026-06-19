#!/usr/bin/env bash
# PostToolUse hook: run clang-format on a single C++ file after Edit/Write.
# Reads tool input JSON on stdin, filters to foo_hemusic/{src,tests} C++
# sources, formats in place, and notifies Claude when the file changed so its
# view stays in sync.

set -uo pipefail

input="$(cat)"
file="$(printf '%s' "$input" | jq -r '.tool_input.file_path // .tool_response.filePath // empty')"
[ -z "$file" ] && exit 0

norm="${file//\\//}"
case "$norm" in
  */foo_hemusic/src/*|*/foo_hemusic/tests/*) ;;
  *) exit 0 ;;
esac
case "$norm" in
  *.cpp|*.cxx|*.cc|*.h|*.hpp|*.hxx) ;;
  *) exit 0 ;;
esac

[ -f "$file" ] || exit 0

# Resolve clang-format. Claude Code spawns hooks with a narrow PATH that may
# omit the LLVM bin dir, so fold in $CLANG_TOOLS_DIR (set in settings) before
# probing. `command -v` resolves the .exe suffix on Windows; no-op if absent.
[ -n "${CLANG_TOOLS_DIR:-}" ] && PATH="$CLANG_TOOLS_DIR:$PATH"
fmt="$(command -v clang-format 2>/dev/null || true)"
[ -z "$fmt" ] && exit 0

before_hash="$(sha1sum "$file" | awk '{print $1}')"
"$fmt" -i "$file" 2>/dev/null || exit 0
after_hash="$(sha1sum "$file" | awk '{print $1}')"

if [ "$before_hash" != "$after_hash" ]; then
  ctx=$'clang-format reformatted '"$(basename "$file")"$' in place. Re-read the file before further edits — its contents on disk no longer match what you wrote.'
  jq -n --arg ctx "$ctx" '{
    hookSpecificOutput: {
      hookEventName: "PostToolUse",
      additionalContext: $ctx
    }
  }'
fi

exit 0
