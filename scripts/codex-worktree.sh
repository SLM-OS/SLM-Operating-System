#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  scripts/codex-worktree.sh <branch-name> [base-ref]

Creates a sibling git worktree on a new branch, then launches Codex
inside that worktree. This keeps the main checkout clean by default.

Examples:
  scripts/codex-worktree.sh feature-eviction-loader
  scripts/codex-worktree.sh fix/lua-demo origin/main
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

branch="${1:-}"
base="${2:-origin/main}"

if [[ -z "$branch" ]]; then
    usage >&2
    exit 1
fi

if ! git rev-parse --show-toplevel >/dev/null 2>&1; then
    echo "error: run this from inside the SLM-OS git checkout" >&2
    exit 1
fi

repo_root="$(git rev-parse --show-toplevel)"
repo_name="$(basename "$repo_root")"
parent_dir="$(dirname "$repo_root")"
worktree_path="${parent_dir}/${repo_name}-${branch//\//-}"

if [[ -e "$worktree_path" ]]; then
    echo "error: worktree path already exists: $worktree_path" >&2
    exit 1
fi

cd "$repo_root"

git fetch origin
git worktree add "$worktree_path" -b "$branch" "$base"

echo "Created worktree:"
echo "  branch:   $branch"
echo "  base:     $base"
echo "  path:     $worktree_path"

cd "$worktree_path"
exec codex
