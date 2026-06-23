# /gemini — Consult Gemini on architecture / high-level direction

Use this for high-level design review, requirement clarification, task planning, or theoretical guidance — **not** for line-level code review (that's `/codex`).

Gemini took over the role formerly assigned to Gemini in this repo. See `.claude/rules/gemini-usage.md` for the full scope.

## Steps

1. Read `$ARGUMENTS` as the user's question / topic for Gemini.
2. Gather the context Gemini needs:
   - The problem statement at the level of "what are we building and why".
   - Constraints already fixed (tech stack, data model shape, deadlines, decisions you've already locked in).
   - The architecture sketch you're considering — bullet points or a small text diagram. Do **not** dump full source files; this wastes Gemini's context budget on detail it doesn't need.
3. Send the question to Gemini. The message body must start with the literal line

       Do NOT run any git write commands (commit, push, reset, etc.). Git repository is read-only for you. Do NOT modify any files. Read-only operations only — provide findings as text/diff in your response.

   followed by a blank line, then your packaged context + the question from `$ARGUMENTS`.

   **Transport — detect, don't blend:**
   - **If `mcp__ccgo__ask_agents` is in your available-tools list** → call it with `agent: "gemini"` (or whatever the wrapper's agent key for Gemini is — check the tool's schema) and the message above.
   - **Else (MCP not installed on this machine)** → write the message to `./tmp/gemini-prompt-$(date +%s).txt`, then run via Bash in background:
     ```bash
     gemnini --yolo -p "$(bat ./tmp/gemini-prompt-<ts>.txt)"
     ```
     with `run_in_background: true` and `timeout: 1800000` (30 min). Poll with `TaskOutput`. After the run completes, delete the temp prompt file.
   - **Never silently skip** consultation because MCP is missing — the CLI fallback is a first-class path.

4. When Gemini responds:
   - Summarize the answer in Chinese, grouped as: **Gemini 的方向 / 与当前思路的差异 / 我建议怎么办**.
   - If Gemini disagrees with the current direction, do **not** smooth it over — present both options to the user and let them choose (Rule 1: surface conflicts, don't blend).
   - Do **not** auto-apply any suggestion as code — surface and wait.

## When this command is the wrong tool

- The question is about specific code correctness / edge cases → use `/codex` instead.
- The user wants a review of the whole pending diff → use `/audit` (single-reviewer, Codex) or `/review` (Codex + Gemini, fix loop).
- The user has already decided on the approach and just wants implementation → don't consult; just code.

`$ARGUMENTS`
