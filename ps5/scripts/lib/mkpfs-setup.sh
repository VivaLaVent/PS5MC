# Fetch the MkPFS packer (github.com/PSBrew/MkPFS) into .deps and echo a runner
# that invokes `python -m mkpfs`. Pinned to the exact commit, as ProsperoLight
# does. Sourced or run; prints the runner path on stdout, diagnostics on stderr.
#   usage: MKPFS=$(bash scripts/lib/mkpfs-setup.sh) && "$MKPFS" pack folder ...
set -euo pipefail
REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
MKPFS_REV="6cb8313dfe0c988ac52617794553f343243d3a56"
checkout="$REPO_ROOT/.deps/MkPFS"

for c in git python3; do command -v "$c" >/dev/null || { echo "missing required command: $c" >&2; exit 2; }; done

if [ ! -d "$checkout/.git" ]; then
  mkdir -p "$checkout"
  git -C "$checkout" init --quiet
  git -C "$checkout" remote add origin https://github.com/PSBrew/MkPFS.git 2>/dev/null || true
  git -C "$checkout" fetch --quiet --depth 1 origin "$MKPFS_REV"
  git -C "$checkout" checkout --quiet --detach FETCH_HEAD
fi
actual=$(git -C "$checkout" rev-parse HEAD)
[ "$actual" = "$MKPFS_REV" ] || { echo "MkPFS cache is at $actual; expected $MKPFS_REV" >&2; exit 2; }

venv="$checkout/.venv-linux"
python="$venv/bin/python"
stamp="$venv/.installed-revision"
if [ ! -x "$python" ]; then
  rm -rf -- "$venv"
  python3 -m venv "$venv" || { echo "python3-venv is required" >&2; exit 2; }
fi
"$python" -m pip --version >/dev/null 2>&1 || python3 -m pip --python "$python" install --disable-pip-version-check --quiet pip >&2
if [ ! -f "$stamp" ] || [ "$(cat "$stamp")" != "$MKPFS_REV" ]; then
  "$python" -m pip install --disable-pip-version-check --quiet "$checkout" >&2
  printf '%s\n' "$MKPFS_REV" > "$stamp"
fi

runner="$checkout/.mkpfs-run"
printf '#!/bin/sh\nexec "%s" -m mkpfs "$@"\n' "$python" > "$runner"
chmod +x "$runner"
"$runner" --help >/dev/null
printf '%s\n' "$runner"
