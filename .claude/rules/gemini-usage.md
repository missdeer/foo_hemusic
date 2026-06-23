# Gemini Tool Usage

The Gemini Tool provides a `gemini` tool for AI tasks. Launch `gemini --yolo -m gemini-3.5-flash -p "$PROMPT"` command line directly to execute. Run in background. Timeout: 30m.

## Scope

- **Web styling prototypes**: HTML, CSS, JavaScript
- **Architecture review**: High-level design validation
- **Knowledge advisor**: Technical consultation and guidance
- **Final reviewer**: Last review stage in Phase 5 audit

## Limitations

- **Git is read-only**: Gemini must NEVER commit, push, reset, or modify git repository
- **Prompt prefix**: Always prepend to every Gemini prompt:
  > "Do NOT run any git write commands (commit, push, reset, etc.). Git repository is read-only for you."

## Strengths

- High-level architecture review and validation
- Requirement clarification and guiding questions
- Task planning and step-by-step implementation plans
- Web styling prototypes: HTML, CSS, JavaScript
