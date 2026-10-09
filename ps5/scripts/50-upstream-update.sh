#!/usr/bin/env bash
# 50-upstream-update.sh - take in upstream Kodi updates (docs/FORK-PLAN.md:
# the PS5 work is one linear series of commits, rebased onto each base).
#
# For each branch (ps5mc-piers on Kodi's Piers = 22, ps5mc-omega on Omega =
# 21) it reports:
#   - how many commits upstream has added since the branch's base,
#   - which of the files they change our series changes too (where conflicts
#     can only come from),
#   - whether the series rebases onto them cleanly - tried in a scratch
#     worktree, so the real branches are not touched.
#
#   bash ps5/scripts/50-upstream-update.sh              report + trial rebase
#   bash ps5/scripts/50-upstream-update.sh --apply      also rebase the real
#        branch(es) that rebase cleanly, after tagging their current head
#        (pre-rebase-<date>-<branch>); pushing stays yours (command printed)
#   ONLY=22 | ONLY=21     one branch only
#   ONTO=22.0-Piers       rebase onto a release tag instead of the branch head
#                         (use with ONLY: a tag belongs to one branch)
#   UPSTREAM_URL=...      another upstream (default: github.com/xbmc/xbmc)
#
# Exit status: 0 = everything up to date or clean; 1 = a conflict, a failed
# step, or a dirty tree.
set -uo pipefail

APPLY=0
for arg in "$@"; do
  case "$arg" in
    --apply) APPLY=1 ;;
    -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
    *) echo "!! unknown argument: $arg"; exit 1 ;;
  esac
done
REMOTE="${UPSTREAM_REMOTE:-upstream}"
URL="${UPSTREAM_URL:-https://github.com/xbmc/xbmc.git}"

[ -f xbmc/platform/ps5/main.cpp ] || { echo "!! run inside the PS5MC fork clone"; exit 1; }
git checkout -q -- xbmc/platform/ps5/BuildStamp.h 2>/dev/null || true
if [ -n "$(git status --porcelain)" ]; then
  echo "!! uncommitted changes; commit or stash them first:"; git status --short | head; exit 1
fi
START="$(git rev-parse --abbrev-ref HEAD)"

if ! git remote get-url "$REMOTE" >/dev/null 2>&1; then
  git remote add "$REMOTE" "$URL" && echo "==> added remote '$REMOTE' ($URL)"
fi
echo "==> fetching $REMOTE (Piers, Omega, tags)"
git fetch -q "$REMOTE" Piers Omega --tags || { echo "!! fetch from $REMOTE failed"; exit 1; }

PAIRS=()
[ "${ONLY:-22}" = 22 ] && PAIRS+=("ps5mc-piers:Piers")
[ "${ONLY:-21}" = 21 ] && PAIRS+=("ps5mc-omega:Omega")
if [ -n "${ONTO:-}" ] && [ ${#PAIRS[@]} -ne 1 ]; then
  echo "!! ONTO=$ONTO names one release: set ONLY=21 or ONLY=22 too"; exit 1
fi

FAILED=0
REBASED=()
for pair in "${PAIRS[@]}"; do
  branch="${pair%%:*}"
  up="$REMOTE/${pair##*:}"
  onto="${ONTO:-$up}"
  echo
  echo "################ $branch  (upstream $up)"
  if ! git rev-parse -q --verify "$branch" >/dev/null; then
    echo "   !! no local branch $branch"; FAILED=1; continue
  fi
  if ! git rev-parse -q --verify "$onto^{commit}" >/dev/null; then
    echo "   !! $onto is not a known commit or tag"; FAILED=1; continue
  fi
  base="$(git merge-base "$branch" "$up")"
  ours="$(git rev-list --count "$base..$branch")"
  new="$(git rev-list --count "$base..$onto")"
  echo "   base:     $(git log -1 --format='%h %ad' --date=short "$base") ($(git describe --tags "$base" 2>/dev/null || echo untagged))"
  echo "   ours:     $ours PS5 commits on top"
  if git merge-base --is-ancestor "$onto" "$base"; then
    echo "   upstream: nothing new in $onto - up to date"; continue
  fi
  echo "   upstream: $new new commits in $onto, newest first:"
  git log --format='     %h %ad %s' --date=short "$base..$onto" | head -8 | cut -c1-110
  [ "$new" -gt 8 ] && echo "     ... and $((new - 8)) more"

  overlap="$(comm -12 <(git diff --name-only "$base" "$onto" | sort) \
                      <(git diff --name-only "$base" "$branch" | sort))"
  if [ -n "$overlap" ]; then
    echo "   files both sides change ($(echo "$overlap" | wc -l)) - conflicts can only come from these:"
    echo "$overlap" | sed 's/^/     /'
  else
    echo "   files both sides change: none"
  fi

  # trial rebase in a scratch worktree
  wt="$(mktemp -d)"
  git worktree add -q --detach "$wt" "$branch" 2>/dev/null || { echo "   !! cannot create a scratch worktree"; FAILED=1; continue; }
  if git -C "$wt" -c user.name=trial -c user.email=trial@localhost rebase -q --onto "$onto" "$base" >/dev/null 2>&1; then
    echo "   trial rebase: CLEAN ($ours commits replayed onto $onto)"
    clean=1
  else
    echo "   !! trial rebase: CONFLICTS"
    git -C "$wt" diff --name-only --diff-filter=U | sed 's/^/     conflict: /'
    stopped="$(git -C "$wt" log -1 --format='%s' REBASE_HEAD 2>/dev/null)"
    [ -n "$stopped" ] && echo "     at our commit: $stopped"
    git -C "$wt" rebase --abort >/dev/null 2>&1
    clean=0
    FAILED=1
  fi
  git worktree remove --force "$wt" >/dev/null 2>&1; rm -rf "$wt"

  if [ "$APPLY" = 1 ] && [ "$clean" = 1 ]; then
    tag="pre-rebase-$(date +%Y%m%d)-$branch"
    git tag -f "$tag" "$branch" >/dev/null
    git checkout -q "$branch" && \
    if git rebase -q --onto "$onto" "$base" >/dev/null 2>&1; then
      echo "   + rebased $branch onto $onto (old head kept as tag $tag)"
      REBASED+=("$branch")
    else
      git rebase --abort >/dev/null 2>&1
      echo "   !! the real rebase failed although the trial was clean; $branch unchanged"; FAILED=1
    fi
  elif [ "$APPLY" = 1 ]; then
    echo "   (not rebased: resolve the conflicts by hand - git rebase --onto $onto $base $branch)"
  fi
done

git checkout -q "$START" 2>/dev/null
if [ ${#REBASED[@]} -gt 0 ]; then
  echo
  echo "==> next: build and test, then publish the rebased branch(es):"
  echo "   bash ps5/scripts/40-release.sh <version> --no-publish"
  echo "   git push --force-with-lease origin ${REBASED[*]}"
  echo "   (release tags keep the old history reachable; the pre-rebase tags are local)"
fi
exit $FAILED
