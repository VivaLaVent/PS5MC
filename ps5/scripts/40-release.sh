#!/usr/bin/env bash
# Build and package BOTH PS5MC variants and publish one GitHub release with two
# assets. Run from inside the PS5MC fork clone on a clean tree.
#
#   PS5MC-<ver>-kodi22-PPSA99420.zip   from ps5mc-piers  (title "PS5MC")
#   PS5MC-<ver>-kodi21-PPSA99421.zip   from ps5mc-omega  (title "PS5MC (Kodi 21)")
#
# Different title IDs are deliberate: the two builds must never share
# /download0 - Kodi only migrates its databases forward, so a 22 -> 21
# downgrade on shared data corrupts the library. Separate IDs let both be
# installed side by side.
#
# Each variant builds in its own build + stage dir, so nothing leaks between
# them. A variant that does not produce an eboot aborts the release: it is two
# zips or nothing (a release with one asset would be a different product).
#
# Usage:  bash ps5/scripts/40-release.sh 1.0            # build + zip + release
#         bash ps5/scripts/40-release.sh 1.0 --no-publish   # build + zip only
#         ONLY=22 bash ps5/scripts/40-release.sh 1.0 --no-publish   # one variant, for iterating
set -uo pipefail
VER="${1:?usage: 40-release.sh <version> [--no-publish]}"; shift || true
PUBLISH=1; [ "${1:-}" = "--no-publish" ] && PUBLISH=0
FORK="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"   # the Kodi tree (ps5/scripts/../..)
SCRIPTS="$FORK/ps5/scripts"
OUT="${OUT:-$HOME/ps5mc-releases/$VER}"; mkdir -p "$OUT"
ONLY="${ONLY:-}"

cd "$FORK"
[ -z "$(git status --porcelain)" ] || { echo "!! fork tree not clean; commit first (the stamp must name a commit)"; exit 1; }
START_BRANCH="$(git rev-parse --abbrev-ref HEAD)"

# variant: name  branch        title-id   on-screen name        build dir              stage dir
VARIANTS=(
  "kodi22 ps5mc-piers PPSA99420 PS5MC_(Kodi_22)  $HOME/ps5mc-build-22 $HOME/ps5mc-stage-22"
  "kodi21 ps5mc-omega PPSA99421 PS5MC_(Kodi_21)  $HOME/ps5mc-build-21 $HOME/ps5mc-stage-21"
)

built=()
for v in "${VARIANTS[@]}"; do
  set -- $v; name=$1 branch=$2 tid=$3 tname="${4//_/ }" bdir=$5 sdir=$6
  [ -n "$ONLY" ] && [ "$ONLY" != "${name#kodi}" ] && { echo "==> skipping $name (ONLY=$ONLY)"; continue; }
  echo "################ $name: $branch -> $tid \"$tname\""
  git checkout -q "$branch" || { echo "!! cannot check out $branch"; exit 1; }
  # Kodi 21 needs lzo2 in the sysroot (no internal build on 21); idempotent.
  [ "$name" = kodi21 ] && { bash "$SCRIPTS/22-build-kodi21-deps.sh" > "$OUT/$name-deps.log" 2>&1 || { echo "!! $name: sysroot deps failed - see $OUT/$name-deps.log"; grep '!!' "$OUT/$name-deps.log"; exit 1; }; }
  # configure + build + deploy, each variant fully isolated
  KODI_SRC="$FORK" BUILD="$bdir" bash "$SCRIPTS/20-configure-kodi.sh" > "$OUT/$name-configure.log" 2>&1 \
    || { echo "!! $name: configure failed - see $OUT/$name-configure.log"; grep -E '!!' "$OUT/$name-configure.log"; exit 1; }
  grep -E '==> (source mode|build stamp)' "$OUT/$name-configure.log"
  # KEEP_GOING=1: don't stop at the first failing file - report every error in
  # one run (for porting rounds; the artifact checks below still gate success).
  cmake --build "$bdir" -j"$(nproc)" ${KEEP_GOING:+-- -k 0} > "$OUT/$name-build.log" 2>&1 \
    || { echo "!! $name: build failed:"; grep -nE 'error:|FAILED' "$OUT/$name-build.log" | head; exit 1; }
  KODI_SRC="$FORK" BUILD="$bdir" STAGE="$sdir" TITLE_ID="$tid" TITLE_NAME="$tname" \
    bash "$SCRIPTS/30-deploy.sh" > "$OUT/$name-deploy.log" 2>&1
  grep -q 'Build complete' "$OUT/$name-deploy.log" || { echo "!! $name: deploy/package failed - see $OUT/$name-deploy.log"; grep '!!' "$OUT/$name-deploy.log"; exit 1; }
  DIST="$sdir/app/dist/$tid"
  # verify the artifact before zipping: eboot present, Python stdlib present, title id in param.json
  [ -s "$DIST/eboot.bin" ] || { echo "!! $name: no eboot.bin in $DIST"; exit 1; }
  [ -f "$DIST/share/kodi/python/lib/python3.14/os.py" ] || { echo "!! $name: Python stdlib missing from $DIST"; exit 1; }
  grep -q "\"$tid\"" "$DIST/sce_sys/param.json" || { echo "!! $name: param.json does not carry $tid"; exit 1; }
  ZIP="$OUT/PS5MC-$VER-$name-$tid.zip"
  rm -f "$ZIP"; ( cd "$sdir/app/dist" && zip -qr "$ZIP" "$tid" )
  echo "==> $name: $(basename "$ZIP") ($(stat -c%s "$ZIP") bytes, eboot $(stat -c%s "$DIST/eboot.bin") bytes)"
  built+=("$ZIP")
done
git checkout -q "$START_BRANCH"

echo
if [ -z "$ONLY" ] && [ "${#built[@]}" -ne 2 ]; then echo "!! expected 2 zips, have ${#built[@]} - not publishing"; exit 1; fi
if [ "$PUBLISH" = 1 ]; then
  [ -z "$ONLY" ] || { echo "!! ONLY= builds are never published"; exit 1; }
  NOTES="$FORK/ps5/docs/release-notes/$VER.md"
  [ -f "$NOTES" ] || { echo "!! write $NOTES first (see ps5/docs/release-notes/0.9.md for the format)"; exit 1; }
  git tag -a "$VER" -m "PS5MC $VER" 2>/dev/null || true
  git push -q origin "$VER"
  gh release create "$VER" "${built[@]}" --title "PS5MC $VER" --notes-file "$NOTES"
else
  echo "==> zips ready in $OUT (not published):"; printf '    %s\n' "${built[@]}"
fi
