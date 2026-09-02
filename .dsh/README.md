# `.dsh/` — agent configuration

Project-scoped configuration for coding agents. Recognised automatically by
[DSH](https://github.com/deepseek-harness/deepseek-harness); the layout is
compatible with other agent tooling that reads `AGENTS.md` plus a skills
directory.

## Layout

```
AGENTS.md              # project root: baseline instructions, always in context
.dsh/skills/<name>/
  SKILL.md             # one skill; frontmatter + Markdown body
```

## Discovery

DSH walks up from the working directory to the nearest `.git` to find the
project root, then reads skill roots in this precedence order:

| Rank | Root | Scope |
| --- | --- | --- |
| 100 | `<project>/.dsh/skills` | this project (**used here**) |
| 200 | `<project>/.agents/skills` | this project, alternative location |
| 400 | `~/.dsh/skills` | the user, all projects |
| 500 | `~/.agents/skills` | the user, all projects |

Lower rank wins, so a project skill overrides a user skill with the same name.
`.agents/skills` works identically if you prefer it; do not use both.

A skill is either a directory containing `SKILL.md` or a flat `<name>.md` file.
Only those two depths are scanned — nesting deeper is not discovered.

Discovery is live: adding or editing a `SKILL.md` updates the running agent's
catalog without a restart.

## Skill format

```markdown
---
name: oemu-run-tests
description: Use when running, filtering, or debugging oemu's GoogleTest suite - ...
---

# Title

Body in Markdown.
```

Frontmatter fields:

| Field | Required | Notes |
| --- | --- | --- |
| `name` | yes | kebab-case, `^[a-z0-9]+(-[a-z0-9]+)*$`; must match the directory name |
| `description` | yes | **the trigger**: when to load this skill, in the third person |
| `whenToUse` | no | extra selection guidance |
| `disable-model-invocation` | no | `true` hides it from the model; user-invocable only |
| `user-invocable` | no | `false` prevents direct user invocation |

An invalid `name`, or a missing `name`/`description`, makes the skill **silently
ignored** — it just never appears. If a new skill does not show up, that is the
first thing to check.

Only `name` and `description` are loaded up front; the body is fetched on
demand. So the description carries the whole routing decision — write it as
concrete trigger conditions, not as a summary of the contents.

## Skills here

| Skill | Covers |
| --- | --- |
| `oemu-run-tests` | running and filtering tests, debugging failures, CTest labels |
| `oemu-build-configs` | presets, sanitizers, coverage, configure-time problems |
| `oemu-add-c-module` | the four conventions that keep C code testable from C++ |
| `oemu-ci-workflow` | GitHub Actions, reproducing CI-only failures locally |

`AGENTS.md` in the project root holds the always-loaded baseline and points at
these.

## Adding a skill

1. `mkdir -p .dsh/skills/<name>` and write `SKILL.md` with matching `name`.
2. Make `description` state the trigger conditions explicitly.
3. Verify any command you document actually runs in this environment — an
   instruction that fails is worse than none.
4. Add a row to the table above and to `AGENTS.md` if it is a primary workflow.
