# Mirror a title folder to the console over FTP and VERIFY every file landed at
# the right size. A plain mirror (lftp, or FileZilla drag-and-drop) can silently
# drop files in deep trees - that is how skin.estuary/media/Textures.xbt went
# missing and left the UI without icons for several debugging rounds. This
# refuses to report success unless every file's size on the console matches the
# build.
#   usage: PS5_HOST=192.168.x.x deploy_to_console <local_dist_dir> <title_id>
deploy_to_console() {
  local DIST="$1" TID="$2"
  local HOST="${PS5_HOST:?PS5_HOST not set}" PORT="${PS5_FTP_PORT:-2121}"
  local REMOTE="${PS5_HOMEBREW:-/data/homebrew}/$TID"
  command -v lftp >/dev/null || { echo "!! lftp not installed (apt-get install lftp)"; return 1; }
  command -v curl >/dev/null || { echo "!! curl not installed"; return 1; }

  echo "==> mirroring $DIST -> ftp://$HOST:$PORT$REMOTE"
  # --delete removes stale files; -P5 parallel; --loop retries a failed mirror
  # pass once more (lftp's own retry of dropped transfers).
  lftp -e "set net:max-retries 3; set net:timeout 20; mirror -R --delete --parallel=5 '$DIST' '$REMOTE'; mirror -R '$DIST' '$REMOTE'; quit" -p "$PORT" "$HOST" || {
    echo "!! lftp mirror reported an error"; return 1; }

  echo "==> verifying every file's size on the console"
  local bad=0 n=0
  while IFS= read -r f; do
    local rel="${f#$DIST/}"
    local want; want=$(stat -c%s "$f")
    local got; got=$(curl -s -I "ftp://$HOST:$PORT$REMOTE/$rel" 2>/dev/null | awk '/Content-Length|^Content-Length/{print $2}' | tr -d '\r')
    # curl -I over FTP may not give size; fall back to a sized download probe
    [ -z "$got" ] && got=$(curl -s "ftp://$HOST:$PORT$REMOTE/$rel" 2>/dev/null | wc -c)
    n=$((n+1))
    if [ "$got" != "$want" ]; then
      echo "   !! $rel: console has ${got:-0} bytes, build has $want"
      bad=$((bad+1))
    fi
  done < <(find "$DIST" -type f)

  if [ "$bad" -ne 0 ]; then
    echo "!! $bad of $n files did not transfer correctly - the install is INCOMPLETE."
    echo "   (re-run; if it persists, check the FTP server and free space on the console)"
    return 1
  fi
  echo "==> all $n files verified on the console"
}
