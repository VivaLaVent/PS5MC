#!/usr/bin/env bash
# Build and package BOTH PS5MC variants and publish one GitHub release. Run from
# inside the PS5MC fork clone on a clean tree.
#
#   PS5MC-<ver>-kodi22-PPSA99420.{zip,exfat}   from ps5mc-piers  ("PS5MC (Kodi 22)")
#   PS5MC-<ver>-kodi21-PPSA99421.{zip,exfat}   from ps5mc-omega  ("PS5MC (Kodi 21)")
#
# Different title IDs are deliberate: the two builds must never share Kodi's
# data - Kodi only migrates its databases forward, so a 22 -> 21 downgrade on
# shared data corrupts the library. Separate IDs (and separate /data homes) let
# both be installed side by side.
#
# Each variant builds in its own build + stage dir, so nothing leaks between
# them. A variant that does not produce its assets aborts the release: both
# variants or nothing (a release with one would be a different product).
#
# Usage:  bash ps5/scripts/40-release.sh 1.0                  # build + package + release
#         bash ps5/scripts/40-release.sh 1.0 --no-publish     # build + package only
#         ONLY=22 bash ps5/scripts/40-release.sh 1.0 --no-publish   # one variant, for iterating
#         NO_EXFAT=1 ...                                      # skip the exFAT image (iterating)
set -uo pipefail
VER="${1:?usage: 40-release.sh <version> [--no-publish]}"; shift || true
PUBLISH=1; [ "${1:-}" = "--no-publish" ] && PUBLISH=0
FORK="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"   # the Kodi tree (ps5/scripts/../..)
SCRIPTS="$FORK/ps5/scripts"
OUT="${OUT:-$HOME/ps5mc-releases/$VER}"; mkdir -p "$OUT"
ONLY="${ONLY:-}"

cd "$FORK"
STAMP_FILE=xbmc/platform/ps5/BuildStamp.h
# BuildStamp.h is generated per build (lib/write-build-stamp.sh); a build that
# failed earlier may have left it modified. It is never a real change: reset it.
git checkout -q -- "$STAMP_FILE" 2>/dev/null || true
[ -z "$(git status --porcelain)" ] || { echo "!! fork tree not clean; commit first (the stamp must name a commit)"; git status --short | head; exit 1; }
START_BRANCH="$(git rev-parse --abbrev-ref HEAD)"
# On ANY exit (success, failure, Ctrl-C): reset the stamp and return to the
# branch we started on, so a failed build never leaves the tree dirty or the
# checkout on the wrong branch for the next run.
trap 'git -C "$FORK" checkout -q -- "$STAMP_FILE" 2>/dev/null; git -C "$FORK" checkout -q "$START_BRANCH" 2>/dev/null' EXIT

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
  grep -E '==> (source mode|source:|host tools)' "$OUT/$name-configure.log"
  # KEEP_GOING=1: don't stop at the first failing file - report every error in
  # one run (for porting rounds; the artifact checks below still gate success).
  KODI_SRC="$FORK" bash "$SCRIPTS/lib/write-build-stamp.sh"
  cmake --build "$bdir" -j"$(nproc)" ${KEEP_GOING:+-- -k 0} > "$OUT/$name-build.log" 2>&1 \
    || { echo "!! $name: build failed (full log: $OUT/$name-build.log):"
         # '!!'-prefixed so the cause survives a filter on the output; the failed
         # target and the compiler's own messages, with source paths shortened
         grep -E '^FAILED| error: ' "$OUT/$name-build.log" | head -15 | sed -E "s|$FORK/||g" | cut -c1-240 | sed 's/^/!!   /'
         exit 1; }
  # contentVersion for the store: 01.000.0NN from the numeric release version
  # (1.2 -> 01.000.012, 1.11 -> 01.000.011... keep tags <= x.99). Rises per tag.
  CVER=$(printf '01.000.0%02d' "$(printf '%s' "$VER" | awk -F. '{printf (($1*10)+$2)%100}')")
  # The deploy's exit status decides: its checks (e.g. the texture-bundle
  # version check) run AFTER the eboot step has printed "Build complete".
  if ! KODI_SRC="$FORK" BUILD="$bdir" STAGE="$sdir" TITLE_ID="$tid" TITLE_NAME="$tname" CONTENT_VERSION="$CVER" \
       bash "$SCRIPTS/30-deploy.sh" > "$OUT/$name-deploy.log" 2>&1 ||
     ! grep -q 'Build complete' "$OUT/$name-deploy.log"; then
    echo "!! $name: deploy/package failed (full log: $OUT/$name-deploy.log):"
    grep '!!' "$OUT/$name-deploy.log" | head -15 | sed 's/^/!!   /'
    exit 1
  fi
  grep -E 'texture bundles:' "$OUT/$name-deploy.log" | sed "s/^ */==> $name: /"
  DIST="$sdir/app/dist/$tid"
  # verify the artifact before zipping: eboot present, Python stdlib present, title id in param.json
  [ -s "$DIST/eboot.bin" ] || { echo "!! $name: no eboot.bin in $DIST"; exit 1; }
  [ -f "$DIST/share/kodi/python/lib/python3.14/os.py" ] || { echo "!! $name: Python stdlib missing from $DIST"; exit 1; }
  grep -q "\"$tid\"" "$DIST/sce_sys/param.json" || { echo "!! $name: param.json does not carry $tid"; exit 1; }
  # Checksum manifest INSIDE the title folder, so the install can be verified
  # after any transfer (FileZilla drag-and-drop silently drops files in deep
  # trees - this is how skin.estuary/media/Textures.xbt once went missing).
  ( cd "$DIST" && find . -type f ! -name MANIFEST.sha256 -print0 | sort -z       | xargs -0 sha256sum > MANIFEST.sha256 )
  echo "==> $name: MANIFEST.sha256 lists $(wc -l < "$DIST/MANIFEST.sha256") files"
  ZIP="$OUT/PS5MC-$VER-$name-$tid.zip"
  rm -f "$ZIP"; ( cd "$sdir/app/dist" && zip -qr "$ZIP" "$tid" )
  echo "==> $name: $(basename "$ZIP") ($(stat -c%s "$ZIP") bytes, eboot $(stat -c%s "$DIST/eboot.bin") bytes)"
  built+=("$ZIP")
  # Raw exFAT image: a single-file install that cannot partially-drop files over
  # FTP (the folder's many files can). The format BlackBearReloaded ship, as it
  # avoids the PFSC mounting corruption seen on fw 13.60. Zip + exFAT are the
  # only release assets.
  if [ "${NO_EXFAT:-0}" != 1 ]; then
    EXF="$OUT/PS5MC-$VER-$name-$tid.exfat"
    if PY=$(REPO_ROOT="$FORK/ps5" bash "$SCRIPTS/lib/mkpfs-setup.sh" 2>"$OUT/$name-exfat.log") &&
       "$PY" "$SCRIPTS/lib/pack-exfat.py" "$DIST" "$sdir/app/dist/$tid.exfat" >>"$OUT/$name-exfat.log" 2>&1; then
      cp "$sdir/app/dist/$tid.exfat" "$EXF"
      echo "==> $name: $(basename "$EXF") ($(stat -c%s "$EXF") bytes)"
      built+=("$EXF")
    else
      echo "!! $name: .exfat packing failed - see $OUT/$name-exfat.log"; tail -3 "$OUT/$name-exfat.log"
      [ -z "$ONLY" ] && { echo "!! not publishing without the exFAT image"; exit 1; }
    fi
  fi
  git -C "$FORK" checkout -q -- xbmc/platform/ps5/BuildStamp.h 2>/dev/null || true
done
git checkout -q "$START_BRANCH"

echo
zips=0; for a in "${built[@]}"; do [ "${a##*.}" = zip ] && zips=$((zips + 1)); done
if [ -z "$ONLY" ] && [ "$zips" -ne 2 ]; then echo "!! expected both variants (kodi22 + kodi21), have $zips - not publishing"; exit 1; fi
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
