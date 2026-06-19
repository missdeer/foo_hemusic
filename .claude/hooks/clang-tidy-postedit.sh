#!/usr/bin/env bash
# PostToolUse hook: lint a single C++ file after Edit/Write.
# Reads tool input JSON on stdin, filters to tools/crawler-webengine/ C++
# sources, runs clang-tidy with the project's compile_commands.json, and
# injects diagnostics back into Claude's context (non-blocking).

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

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="$repo_root/tools/crawler-webengine/cmake-build"
[ -f "$build_dir/compile_commands.json" ] || exit 0

output="$(clang-tidy --quiet -p "$build_dir" "$file" 2>&1)"

if printf '%s' "$output" | grep -qE '(warning|error):'; then
  ctx=$'clang-tidy diagnostics for '"$(basename "$file")"$':\n\n'"$output"$'\n\nFix these before continuing.'
  jq -n --arg ctx "$ctx" '{
    hookSpecificOutput: {
      hookEventName: "PostToolUse",
      additionalContext: $ctx
    }
  }'
fi

exit 0
