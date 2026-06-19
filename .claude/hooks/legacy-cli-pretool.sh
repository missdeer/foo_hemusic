#!/usr/bin/env bash
# PreToolUse hook on Bash: enforce CLAUDE.md Rule 0 (modern CLI only).
# Reads tool_input.command on stdin; if a legacy UNIX tool is invoked at the
# start of a command segment (start, ;, &&, ||, |, $( ), deny the call and
# tell Claude which modern replacement to use.
#
# Uses bash builtin =~ (ERE) so it does not depend on `rg` being on the hook
# subprocess PATH — Claude Code spawns hooks via cmd.exe → bash, which inherits
# a minimal PATH that does not include rg's install dir on this Windows host.

set -uo pipefail

input="$(cat)"
cmd="$(printf '%s' "$input" | jq -r '.tool_input.command // empty')"
[ -z "$cmd" ] && exit 0

# Strip line continuations so a legacy command on a continued line is still
# evaluated at the start of its logical segment.
cmd_flat="${cmd//$'\\\n'/ }"

violation=""
suggest=""
fix_hint=""

# ERE equivalent of \b at the trailing edge: end-of-string OR non-word char.
# Leading anchor stays the same — bash =~ supports start-of-string ^ and the
# alternation/character-class syntax used here verbatim.
check_legacy() {
  local legacy="$1" modern="$2" hint="$3"
  local pat="(^|[;&|]|\\\$\\()[[:space:]]*([A-Za-z_][A-Za-z0-9_]*=[^[:space:]]+[[:space:]]+)*${legacy}($|[^A-Za-z0-9_])"
  if [[ "$cmd_flat" =~ $pat ]]; then
    violation="$legacy"
    suggest="$modern"
    fix_hint="$hint"
    return 0
  fi
  return 1
}

if check_legacy 'find' 'fd' 'e.g. `fd -e go` instead of `find . -name "*.go"`'; then :
elif check_legacy 'grep' 'rg' 'e.g. `rg PATTERN` instead of `grep -r PATTERN .` (git grep is fine)'; then :
elif check_legacy 'cat'  'bat (or the Read tool for files you want to inspect)' 'e.g. `bat file.txt` or use Read; heredoc to a file should use Write'; then :
elif check_legacy 'ls'   'eza' 'e.g. `eza -la` instead of `ls -la`'; then :
elif check_legacy 'diff' 'delta' 'e.g. `delta a b` (git diff is fine, it produces a diff for delta to render)'; then :
elif [[ "$cmd_flat" =~ (^|[^A-Za-z0-9_])python[0-9.]*($|[^A-Za-z0-9_]) ]] \
  && [[ "$cmd_flat" =~ (json\.(load|loads|dump|dumps|JSONDecoder)|import[[:space:]]+json) ]]; then
  violation="python+json"
  suggest="jq"
  fix_hint='e.g. `jq .field file.json` instead of `python -c "import json; ..."`'
fi

if [ -n "$violation" ]; then
  reason="CLAUDE.md Rule 0: '${violation}' is a legacy UNIX tool — use '${suggest}'. ${fix_hint}. Full mapping: find→fd, grep→rg, cat→bat, ls→eza, diff→delta, JSON→jq. Reissue the Bash call with the modern equivalent; do not retry the legacy command."
  jq -n --arg r "$reason" '{
    hookSpecificOutput: {
      hookEventName: "PreToolUse",
      permissionDecision: "deny",
      permissionDecisionReason: $r
    }
  }'
fi

exit 0
