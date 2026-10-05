# Build / deploy / test workflow (PS5MC fork)

**Two builds, always.** Kodi 22 (`ps5mc-piers`, title PPSA99420, "PS5MC") and
Kodi 21 (`ps5mc-omega`, title PPSA99421, "PS5MC (Kodi 21)"). Each has its own
build dir (`~/ps5mc-build-22|21`) and stage dir (`~/ps5mc-stage-22|21`).
`ps5/scripts/40-release.sh <ver>` builds both, verifies each artifact, zips
them as `PS5MC-<ver>-kodi22-PPSA99420.zip` / `PS5MC-<ver>-kodi21-PPSA99421.zip`
and publishes one GitHub release with both. `--no-publish` to just build;
`ONLY=22` (or 21) to iterate on one variant. A missing variant aborts: it is
two zips or nothing.

The repo paths below say `/mnt/c/kodi-ps5`; in the fork era read `~/PS5MC`
and `ps5/scripts/...`. The console facts are unchanged.


The loop we use every round. Following it exactly is what keeps us from the two
failure modes that have cost whole builds: testing a **stale zip** (an older
download extracted by mistake) and testing a **stale eboot** (a build that
didn't actually relink). Each step below has a check whose job is to fail loudly
rather than let a wrong artifact through.

## Fixed facts

| Thing | Value |
|---|---|
| Console IP | `192.168.66.55` |
| klog port (for `nc`) | `3232` |
| FTP port (file upload) | `2121` |
| Title ID | `PPSA99420` |
| Title folder on console | `/data/homebrew/PPSA99420/` |
| Repo (WSL) | `/mnt/c/kodi-ps5` |
| Build dir (WSL) | `~/kodi-ps5-build` |
| Stage/output dir (WSL) | `~/kodi-ps5-stage/app/dist/PPSA99420/` |
| Downloads (where the zip lands) | `/mnt/c/Users/Gilles/Downloads/` |
| Kodi data on console | `/download0/.kodi` (save data; survives updates) |

## 0. Get the zip (Claude -> you)

Claude packages `kodi-ps5-port.zip` and states its **exact byte size**. You
download it; the browser may name it `kodi-ps5-port(1).zip`, `(2)`, etc. The
size is the only unambiguous identifier, so every extract step verifies it.

## 1. Find the newest zip, verify its size, extract

```bash
EXPECT_BYTES=NNNNNN   # the size Claude gave for THIS zip
Z=$(ls -t /mnt/c/Users/Gilles/Downloads/kodi-ps5-port*.zip | head -1)
echo "newest: $Z  ($(stat -c%s "$Z") bytes, expect $EXPECT_BYTES)"
[ "$(stat -c%s "$Z")" = "$EXPECT_BYTES" ] || { echo "!! SIZE MISMATCH - re-download or wait for the download to finish"; }
cd /mnt/c && unzip -o "$Z" >/dev/null && echo "extracted $Z"
```

If the size mismatches, the download is stale or still in progress; do not
build. `ls -t` picks the newest by mtime; the size check catches the case where
the newest file is still the wrong one.

## 2. Commit (stamp provenance) and build

Committing first is what makes the on-console build stamp (`git describe`)
match the tree, so a wrong eboot is detectable.

```bash
cd /mnt/c/kodi-ps5 && git add -A && git commit -qm 'MESSAGE'; git push -q; git log --oneline -1
cd ~ && bash /mnt/c/kodi-ps5/scripts/20-configure-kodi.sh > ~/kodi-configure.log 2>&1 \
  && grep -iE '==>|!!' ~/kodi-configure.log \
  && cmake --build ~/kodi-ps5-build -j$(nproc) > ~/kodi-build.log 2>&1 \
  && bash /mnt/c/kodi-ps5/scripts/30-deploy.sh 2>&1 | grep 'Build complete\|stamp\|!!'; \
  grep -n 'FAILED:\| error:' ~/kodi-build.log | head
```

- Configure must print `==> N Kodi patches applied` and (for Python builds)
  `PYTHON enabled: Yes`; a `!!` line means it aborted - read `~/kodi-configure.log`.
- `Build complete` from the deploy is the success marker.
- `grep FAILED:/error:` at the end surfaces a compile failure that the `&&`
  chain would otherwise hide.

**When to skip `20-configure`:** only when nothing under `overlay/` or
`patches/` changed (a shim, script, or `pacbrew/` change). Then run just
`cmake --build … && 30-deploy.sh`. When in doubt, run configure.

**Sysroot libraries** (`libkodishim`, `libiconv`, SDK stubs via scripts 12/17/21):
cmake does not track them as dependencies of `kodi.bin`, so after rebuilding
one, those scripts delete `kodi.bin` to force a relink (see
`scripts/lib/sysroot-changed.sh`). Run the relevant `scripts/NN-*.sh` before
`cmake --build`.

## 3. Confirm the eboot is real before deploying

```bash
ls -l --time-style=+%H:%M ~/kodi-ps5-stage/app/dist/PPSA99420/eboot.bin   # fresh timestamp
# Python builds only: stdlib staged
du -sh ~/kodi-ps5-stage/app/dist/PPSA99420/share/kodi/python 2>/dev/null
```

## 4. Deploy to the console

Quit Kodi first (Close Application) - replacing a running title's files can
crash the console.

```bash
PS5=192.168.66.55
# eboot (always)
curl -T ~/kodi-ps5-stage/app/dist/PPSA99420/eboot.bin ftp://$PS5:2121/data/homebrew/PPSA99420/eboot.bin
# a whole folder (e.g. first time shipping Python's stdlib, or add-ons):
cd ~/kodi-ps5-stage/app/dist/PPSA99420 && \
  find share/kodi/python -type f | while read f; do \
    curl -s --ftp-create-dirs -T "$f" "ftp://$PS5:2121/data/homebrew/PPSA99420/$f"; done
```

Verify a folder upload landed:
```bash
curl -s ftp://192.168.66.55:2121/data/homebrew/PPSA99420/share/kodi/python/lib/python3.14/ | head
```

Restart ShadowMountPlus (re-register the title), then launch Kodi.

### Upload block (paste after quitting Kodi)

```bash
PS5=192.168.66.55
D=~/kodi-ps5-stage/app/dist/PPSA99420
curl -s -T "$D/eboot.bin" "ftp://$PS5:2121/data/homebrew/PPSA99420/eboot.bin" \
  && echo "sent eboot.bin ($(stat -c%s "$D/eboot.bin") bytes)"
curl -s "ftp://$PS5:2121/data/homebrew/PPSA99420/" \
  | awk '$NF=="eboot.bin"{print "on console:", $5, "bytes"}'
```

The two sizes must match - that is the check that the new eboot actually
landed. Kodi must be closed first: overwriting a running title's files can
crash the console.

## 5. Capture the console log with nc

Start the capture in a WSL terminal **just before** launching Kodi, leave it
running, Ctrl-C when done. Always write to `~/kodi-py.txt` so the grep steps
find it.

```bash
nc 192.168.66.55 3232 | tee ~/kodi-py.txt
```

`kodi-debug` switch file on the console turns on debug logging (verbose). For a
release smoke test, leave it off for speed; for diagnosis, turn it on.

## 6. Grep the log for the specific thing under test

Always `grep -a` (the log is treated as binary), and `grep -av SSL_DATA` to
drop curl's TLS-record spam. Examples:

```bash
# build stamp matches the commit, and no crash
grep -a "kodi-ps5\] build" ~/kodi-py.txt | tail -1
grep -a -c "A user thread receives a fatal signal" ~/kodi-py.txt

# Python networking / add-on
grep -a -iE "pysock|Traceback|Errno|gismeteo" ~/kodi-py.txt | grep -av SSL_DATA | tail

# video decode
grep -a -iE "videodec2|first picture|software decoding|Decode failed" ~/kodi-py.txt | tail

# binary add-on bring-up
grep -a "test.binary.ps5\|STAGE 2" ~/kodi-py.txt
```

For a crash, symbolize the backtrace (addresses are load base `0x400000` +
offset): subtract `0x400000` from each `# <addr>` line and feed the results to
`llvm-symbolizer-18 --obj=~/kodi-ps5-stage/app/build/llvm-pie.elf -f -i <...>`.

## Batching: one paste per phase

Run whole phases in a single paste, stopping only where a human must look or
act. Three stops: (A) after extract+build - look before deploying; (B) a manual
console action - quit Kodi, FTP upload, restart ShadowMountPlus; (C) after the
on-console capture+grep. Batch A guards itself: it refuses to build on a zip
size mismatch and prints build errors before the deploy line. Within a batch,
chain with `&&` where a later step must not run on failure, `;` where it should
run regardless (e.g. the error grep, which must print even when the build
failed). Keep the three stops - a fully-blind one-paste run hides which step
failed.

## Principles that produced these steps

- **Verify the artifact under test is the intended one** - zip by byte size,
  eboot by stamp/timestamp, a symbol by `grep` inside the file - before drawing
  any conclusion from a run. More than one "the fix didn't work" turned out to
  be a stale zip or an unrelinked eboot.
- **Measure, don't guess.** When something fails, add logging / inspect the
  artifact and let the evidence name the cause, rather than proposing a fix for
  an unconfirmed mechanism.
- **Keep the shipping build safe while diagnosing** - move a file aside, gate a
  change behind a switch file - so the console always has a working eboot.
