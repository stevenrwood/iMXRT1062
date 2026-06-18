#!/usr/bin/env bash
# setup_forks.sh - configure 'fork' remotes on each grblHAL submodule of the
# iMXRT1062 driver, so you can push branches to your personal forks without
# re-typing URLs. Reads firmware-forks.json (alongside this script).
#
# Usage:
#   - Have iMXRT1062 cloned on this machine (e.g. ~/GitHub/iMXRT1062 or
#     somewhere else). Pass its path as the only argument.
#   - Have firmware-forks.json next to this script (it is, in this repo).
#
# Example:
#   ./setup_forks.sh ~/GitHub/iMXRT1062
#   ./setup_forks.sh "C:/Users/you/Documents/code/iMXRT1062"   # Git Bash on Windows
#
# After running, in each submodule:
#   git remote -v   # shows: origin (upstream) + fork (your fork)
#   git push fork your-branch   # pushes to your fork
#
# Works on macOS / Linux / Git Bash on Windows. Requires `jq` for JSON
# parsing — install via 'choco install jq' on Windows, or 'brew install jq'
# on macOS. If jq is not available, the script falls back to Python.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MANIFEST="$SCRIPT_DIR/firmware-forks.json"

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <path-to-iMXRT1062-driver-checkout>" >&2
    echo "  example: $0 ~/GitHub/iMXRT1062" >&2
    exit 1
fi

DRIVER_ROOT="$1"

if [[ ! -d "$DRIVER_ROOT" ]]; then
    echo "ERROR: driver path '$DRIVER_ROOT' does not exist" >&2
    exit 1
fi

if [[ ! -f "$MANIFEST" ]]; then
    echo "ERROR: firmware-forks.json not found next to script ($MANIFEST)" >&2
    exit 1
fi

# Pick a JSON parser
parse_manifest() {
    if command -v jq >/dev/null 2>&1; then
        jq -r '.submodules[] | .path + "|" + .fork' "$MANIFEST"
    elif command -v python3 >/dev/null 2>&1; then
        python3 -c "
import json, sys
with open('$MANIFEST') as f:
    data = json.load(f)
for sm in data['submodules']:
    print(f\"{sm['path']}|{sm['fork']}\")
"
    elif command -v python >/dev/null 2>&1; then
        python -c "
import json, sys
with open('$MANIFEST') as f:
    data = json.load(f)
for sm in data['submodules']:
    print(sm['path'] + '|' + sm['fork'])
"
    else
        echo "ERROR: neither jq nor python found. Install one and re-run." >&2
        exit 1
    fi
}

echo "Setting up 'fork' remotes for submodules in $DRIVER_ROOT"
echo

while IFS='|' read -r sm_path fork_url; do
    full_path="$DRIVER_ROOT/$sm_path"
    if [[ ! -d "$full_path/.git" && ! -f "$full_path/.git" ]]; then
        echo "  [SKIP] $sm_path -- not a git working tree at $full_path"
        continue
    fi

    echo "  [$sm_path]"
    cd "$full_path"

    # Remove any existing 'fork' remote so we can rewrite cleanly
    if git remote | grep -qx fork; then
        git remote remove fork
    fi

    git remote add fork "$fork_url"
    echo "    fork  -> $fork_url"

    # Fetch so the operator can see what branches already exist on the fork
    if git fetch fork --quiet 2>/dev/null; then
        BRANCHES=$(git branch -r --list 'fork/*' | grep -v 'HEAD' | sed 's|^ *fork/||' | paste -sd ", " -)
        if [[ -n "$BRANCHES" ]]; then
            echo "    fork branches: $BRANCHES"
        fi
    else
        echo "    (could not fetch fork — push from local first)"
    fi

    cd "$DRIVER_ROOT"
    echo
done < <(parse_manifest)

echo "Done. To push a new branch from this machine:"
echo "  cd $DRIVER_ROOT/<submodule path>"
echo "  git checkout -B <branch-name> origin/master"
echo "  # make your changes, commit"
echo "  git push -u fork <branch-name>"
echo
echo "Then add an entry to firmware-forks.json under the matching submodule's"
echo "'branches_pushed' list and append a new card to proposedprs.html."
