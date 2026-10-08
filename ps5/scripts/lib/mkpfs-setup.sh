# Fetch the MkPFS packer (github.com/PSBrew/MkPFS) into .deps and echo the path
# to its venv python (which has the mkpfs package installed). Pinned commit.
# Used to build the raw .exfat release image (mkpfs.exfat_writer).
set -euo pipefail
REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
MKPFS_REV="6cb8313dfe0c988ac52617794553f343243d3a56"
checkout="$REPO_ROOT/.deps/MkPFS"
for c in git python3; do command -v "$c" >/dev/null || { echo "missing: $c" >&2; exit 2; }; done
if [ ! -d "$checkout/.git" ]; then
  mkdir -p "$checkout"; git -C "$checkout" init --quiet
  git -C "$checkout" remote add origin https://github.com/PSBrew/MkPFS.git 2>/dev/null || true
  git -C "$checkout" fetch --quiet --depth 1 origin "$MKPFS_REV"
  git -C "$checkout" checkout --quiet --detach FETCH_HEAD
fi
[ "$(git -C "$checkout" rev-parse HEAD)" = "$MKPFS_REV" ] || { echo "MkPFS wrong rev" >&2; exit 2; }
venv="$checkout/.venv-linux"; py="$venv/bin/python"; stamp="$venv/.installed"
[ -x "$py" ] || { rm -rf "$venv"; python3 -m venv "$venv" || { echo "need python3-venv" >&2; exit 2; }; }
if [ ! -f "$stamp" ] || [ "$(cat "$stamp")" != "$MKPFS_REV" ]; then
  "$py" -m pip install --disable-pip-version-check --quiet "$checkout" >&2
  echo "$MKPFS_REV" > "$stamp"
fi
"$py" -c "import mkpfs.exfat_writer" 2>/dev/null || { echo "mkpfs.exfat_writer missing in MkPFS pin" >&2; exit 2; }
printf '%s\n' "$py"
