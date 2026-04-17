# Configuring Claude Code for Git Operations

This guide covers setting up Claude Code to perform Git operations (commits, pushes) only when explicitly requested and approved by the user.

---

## Overview

Claude Code can execute Git commands through its Bash tool, but follows strict safety protocols:

1. **Never commits unless explicitly asked**
2. **Requires user approval for Git operations** (by default)
3. **Never force pushes or runs destructive commands**
4. **Never modifies Git config**

This guide explains how to verify and customize these behaviors.

---

## Default Behavior

By default, Claude Code:

- Will NOT commit changes proactively
- Will ask for permission before running Git commands
- Will show you the exact command before execution
- Will summarize changes before committing

You don't need to change anything to get this behavior — it's the default.

---

## Claude Code Permission System

Claude Code uses a permission system that controls which commands require approval.

### Permission Modes

| Mode | Description |
|------|-------------|
| **Allow** | Command runs without asking |
| **Ask** | Prompts for approval each time |
| **Deny** | Command is blocked |

### Viewing Current Permissions

In Claude Code, type:

```
/permissions
```

This shows your current permission configuration.

---

## Configuring Git Permissions

### Project-Level Settings

Create or edit `.claude/settings.json` in your project root:

```json
{
  "permissions": {
    "allow": [],
    "deny": []
  }
}
```

### Recommended Configuration for Controlled Git Access

To ensure Git operations always require approval:

```json
{
  "permissions": {
    "allow": [
      "Read",
      "Glob",
      "Grep",
      "Write",
      "Edit"
    ],
    "deny": [
      "Bash(git push --force)",
      "Bash(git push -f)",
      "Bash(git reset --hard)",
      "Bash(git clean -fd)"
    ]
  }
}
```

This configuration:

- Allows file operations without prompting
- Leaves Git commands in "Ask" mode (requires approval)
- Explicitly denies destructive Git commands

### Location of Settings Files

| File | Scope |
|------|-------|
| `.claude/settings.json` | Project-specific (committed to repo) |
| `.claude/settings.local.json` | Project-specific (not committed, personal) |
| `~/.claude/settings.json` | User-wide defaults |

---

## How to Request a Commit

When you want Claude Code to commit changes, be explicit:

**Good requests:**

- "Commit these changes with a message describing the toolchain setup"
- "Stage and commit the new documentation files"
- "Create a commit for the work we just did"

**Claude Code will then:**

1. Run `git status` to see changes
2. Run `git diff` to review what's being committed
3. Check recent commit messages for style consistency
4. Show you the proposed commit message
5. Wait for your approval before executing

---

## Commit Message Format

Claude Code uses this format for commits:

```
<Summary of changes>

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude <assistant_id>@anthropic.com
```

You can request a different format if preferred.

---

## Git Safety Protocol

Claude Code follows these safety rules:

### Always Does

- Shows changes before committing
- Uses descriptive commit messages
- Checks for sensitive files (.env, credentials)
- Verifies branch before pushing

### Never Does (Unless Explicitly Requested)

- Commits without being asked
- Pushes to remote without being asked
- Amends commits that have been pushed
- Uses `--force` flags
- Modifies `.gitconfig`
- Skips hooks (`--no-verify`)

### Always Blocked

- Force push to main/master (warns even if requested)
- Interactive commands (`git rebase -i`, `git add -i`)

---

## Example Workflow

### 1. After Making Changes

You: "Please commit the documentation files we created"

### 2. Claude Code Reviews

Claude Code runs:

```bash
git status
git diff
git log --oneline -5
```

### 3. Claude Code Proposes

"I'll commit the following files with this message:

**Files:**
- docs/setup-rust-toolchain.md (new)
- docs/setup-qemu-arm64.md (new)
- docs/setup-clion-arm64-toolchain.md (new)

**Message:**
Add development environment setup documentation

Proceed?"

### 4. You Approve

You: "Yes" or press Enter to approve

### 5. Claude Code Executes

```bash
git add docs/
git commit -m "Add development environment setup documentation

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## Pushing to Remote

Pushing is separate from committing. Claude Code will NOT push unless you explicitly ask:

- "Push to origin"
- "Push these commits to GitHub"

It will then show you what will be pushed and wait for approval.

---

## Useful Git Commands to Request

| Request | What Claude Code Does |
|---------|----------------------|
| "Show git status" | Runs `git status` |
| "What files have changed?" | Runs `git status` and `git diff --stat` |
| "Show the diff" | Runs `git diff` |
| "Commit the changes" | Reviews, proposes message, waits for approval |
| "Push to origin" | Shows commits to push, waits for approval |
| "Create a branch named feature-x" | Runs `git checkout -b feature-x` |

---

## Gitignore Recommendations

Add these to `.gitignore` to avoid committing IDE and personal files:

```gitignore
# IDE
.idea/
*.iml

# Claude Code local settings (personal, not shared)
.claude/settings.local.json

# Build outputs
build/
target/

# OS files
.DS_Store
Thumbs.db
```

Note: `.claude/settings.json` (without `.local`) CAN be committed if you want to share project-level Claude Code settings with collaborators.

---

## Verifying the Setup

### Check Current Configuration

```
/permissions
```

### Test with a Dry Run

Ask Claude Code: "Show me what a commit would look like for the current changes, but don't actually commit"

Claude Code will show the status, diff, and proposed message without executing.

---

## Troubleshooting

### Claude Code commits without asking

This shouldn't happen with default settings. Check:

1. Run `/permissions` to see current config
2. Verify `.claude/settings.json` doesn't have `"Bash(git*)"` in `allow`

### Permission prompts are annoying

You can allow specific safe commands:

```json
{
  "permissions": {
    "allow": [
      "Bash(git status)",
      "Bash(git diff*)",
      "Bash(git log*)"
    ]
  }
}
```

This allows read-only Git commands but still prompts for commits/pushes.

### Claude Code won't commit

Make sure you're explicitly asking. Claude Code errs on the side of caution and won't commit unless clearly requested.

---

## Summary

| Action | Requires Approval |
|--------|-------------------|
| `git status` | Configurable (default: ask) |
| `git diff` | Configurable (default: ask) |
| `git log` | Configurable (default: ask) |
| `git add` | Yes (always) |
| `git commit` | Yes (always) |
| `git push` | Yes (always) |
| `git push --force` | Blocked (warns if requested) |

The default configuration provides exactly what you want: Git operations only happen when you ask and approve.

---

*Last updated: December 2025*
