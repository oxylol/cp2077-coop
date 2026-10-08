#!/usr/bin/env bash
# Type-checks scripts/Cp2077Coop with the redscript compiler's frontend, using hand-written stand-ins for the
# game and Codeware APIs (game-stubs.reds). Catches syntax, type and visibility errors without the game.
#
#   tools/reds-check/check.sh [path-to-redscript-checkout]
#
# Needs git and a Rust toolchain. Clones https://github.com/jac3km4/redscript into .reds-check/ on first use.
# Limits: it can't tell whether a game function really exists or has this signature; game-stubs.reds is only
# as accurate as its declarations.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
redscript="${1:-$repo/.reds-check/redscript}"

if [ ! -d "$redscript" ]; then
    git clone --depth 1 https://github.com/jac3km4/redscript.git "$redscript"
fi
cp "$here/coop.rs" "$redscript/crates/compiler/frontend/tests/coop.rs"

files="$here/game-stubs.reds"
for f in "$repo"/scripts/Cp2077Coop/*.reds; do
    files="$files;$f"
done

output="$(cd "$redscript/crates/compiler/frontend" && COOP_REDS="$files" cargo test -q --test coop -- --nocapture 2>/dev/null)"

# @addMethod(GameInstance) is reported because GameInstance is a stub here; in the game it comes from the
# game's own scripts, where the annotation is allowed (Codeware uses it the same way).
filtered="$(printf '%s\n' "$output" | awk '
    /^\[INVALID_ANN_USE\]/ { skip = 3; next }
    skip > 0 { skip--; next }
    /^\[/ || /^ / || /^[a-z`]/ { print }')"

problems="$(printf '%s\n' "$filtered" | grep -c '^\[' || true)"
printf '%s\n' "$filtered" | grep -v '^COOP_DIAGNOSTICS\|^running\|^test result\|^\.$' || true
echo "redscript check: $problems problem(s)"
[ "$problems" -eq 0 ]
