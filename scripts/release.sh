#!/usr/bin/env bash
# Bumps dkms.conf's PACKAGE_VERSION to a new version, commits, and tags it
# -- the local half of the process documented in RELEASE.md. Requires a
# clean working tree so the version-bump commit only ever contains the
# version bump.
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: scripts/release.sh [VERSION] [--remote NAME]... [--push] [--dry-run]

Bump dkms.conf's PACKAGE_VERSION to VERSION, commit "Bump version to
VERSION", and create annotated tag vVERSION. Requires a clean git working
tree to start.

VERSION can be given as the first argument or via the RELEASE_VERSION
environment variable (the argument wins if both are set). Format:
MAJOR.MINOR.PATCH, optionally with a semver pre-release suffix, e.g.
0.1.0 or 0.1.0-rc0. A suffixed version is meant to be tagged as a
prerelease by the CI release workflows (see RELEASE.md).

Options:
  --remote NAME  Git remote to push the branch and tag to (with --push,
                 or to name in the printed "next steps" otherwise). Can be
                 given more than once to target multiple remotes (e.g.
                 --remote lnx-gitea --remote origin) -- unlike a single
                 canonical upstream, this repo's tag is meant to land on
                 every remote whose CI should build a release from it.
                 Can also be set via the RELEASE_REMOTES environment
                 variable (space-separated; the flag(s) win if both are
                 set). Default: every configured remote.
  --push         Also push the current branch and the new tag.
                 Default: left for you to run yourself (printed at the end).
  --dry-run      Show what would happen without changing anything.
  -h, --help     Show this help.
EOF
}

push=false
dry_run=false
version="${RELEASE_VERSION:-}"
# shellcheck disable=SC2206 # intentional word-splitting of a space-separated env var
remotes=(${RELEASE_REMOTES:-})

while [ $# -gt 0 ]; do
  case "$1" in
    --push) push=true; shift ;;
    --dry-run) dry_run=true; shift ;;
    --remote)
      if [ $# -lt 2 ] || [ -z "$2" ]; then
        echo "error: --remote requires a value" >&2
        exit 1
      fi
      remotes+=("$2")
      shift 2
      ;;
    --remote=*) remotes+=("${1#*=}"); shift ;;
    -h|--help) usage; exit 0 ;;
    -*)
      echo "error: unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
    *) version="$1"; shift ;;
  esac
done

if [ -z "$version" ]; then
  echo "error: no version given - pass it as an argument or set RELEASE_VERSION" >&2
  usage >&2
  exit 1
fi

if ! [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]]; then
  echo "error: version '$version' doesn't look like MAJOR.MINOR.PATCH or MAJOR.MINOR.PATCH-prerelease (e.g. 0.1.0 or 0.1.0-rc0)" >&2
  exit 1
fi

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

if [ "${#remotes[@]}" -eq 0 ]; then
  mapfile -t remotes < <(git remote)
fi

if [ "${#remotes[@]}" -eq 0 ]; then
  echo "error: no git remotes configured, and none given via --remote/RELEASE_REMOTES" >&2
  exit 1
fi

for remote in "${remotes[@]}"; do
  if ! git remote | grep -qx "$remote"; then
    echo "error: remote '$remote' is not configured. Configured remotes:" >&2
    git remote -v >&2
    exit 1
  fi
done

if [ -n "$(git status --porcelain)" ]; then
  echo "error: working tree is not clean - commit, stash, or discard changes first:" >&2
  git status --short >&2
  exit 1
fi

tag="v$version"

if git rev-parse "$tag" >/dev/null 2>&1; then
  echo "error: tag $tag already exists" >&2
  exit 1
fi

conf_file="dkms.conf"
current_version=$(grep -m1 '^PACKAGE_VERSION=' "$conf_file" | cut -d'"' -f2)
needs_bump=true
if [ "$current_version" = "$version" ]; then
  needs_bump=false
fi

echo "Current version: $conf_file PACKAGE_VERSION=$current_version"
echo "New version:     $version"
echo "Remote(s):       ${remotes[*]}"

if $dry_run; then
  if $needs_bump; then
    echo "(dry run) would update $conf_file, commit, and tag $tag"
  else
    echo "(dry run) $conf_file is already at $version; would tag HEAD as $tag directly, no bump commit"
  fi
  exit 0
fi

if $needs_bump; then
  sed -i.bak "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"$version\"/" "$conf_file" && rm -f "$conf_file.bak"
  git add "$conf_file"
  git commit -m "Bump version to $version"
else
  echo "$conf_file is already at $version; tagging HEAD directly (no bump commit needed)."
fi
git tag -a "$tag" -m "$tag"

branch="$(git rev-parse --abbrev-ref HEAD)"

echo
if $needs_bump; then
  echo "Committed and tagged $tag locally on $branch."
else
  echo "Tagged $tag locally on $branch (no new commit needed)."
fi

if $push; then
  for remote in "${remotes[@]}"; do
    git push "$remote" "$branch"
    git push "$remote" "$tag"
    echo "Pushed $branch and $tag to $remote."
  done
else
  echo "Next steps:"
  for remote in "${remotes[@]}"; do
    echo "  git push $remote $branch"
    echo "  git push $remote $tag"
  done
fi
