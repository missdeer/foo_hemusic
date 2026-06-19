#!/usr/bin/env bash
# PostToolUse hook: run clang-format on a single C++ file after Edit/Write.
# Reads tool input JSON on stdin, filters to tools/crawler-webengine/ C++
# sources, formats in place, and notifies Claude when the file was changed
# so its view stays in sync.

set -uo pipefail

input="$(cat)"
file="$(printf '%s' "$input" | jq -r '.tool_input.file_path // .tool_response.filePath // empty')"
[ -z "$file" ] && exit 0

norm="${file//\\//}"
case "$norm" in
  */tools/crawler-webengine/*) ;;
  *) exit 0 ;;
esac
case "$norm" in
  *.cpp|*.cxx|*.cc|*.h|*.hpp|*.hxx) ;;
  *) exit 0 ;;
esac

[ -f "$file" ] || exit 0
command -v clang-format >/dev/null 2>&1 || exit 0

before_hash="$(sha1sum "$file" | awk '{print $1}')"
clang-format -i "$file" 2>/dev/null || exit 0
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
