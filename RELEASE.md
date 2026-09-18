# Release process

Releases are git tags plus, on Gitea and GitHub, a Release object with two
`.deb` files attached: the source DKMS package
(`sinput-dkms_<version>_all.deb`, from
[`scripts/build-source-deb.sh`](scripts/build-source-deb.sh)) and the
precompiled Raspberry Pi 3 binary package
(`sinput-modules-<kernel-release>_<version>_arm64.deb`, from
[`scripts/build-binary-deb.sh`](scripts/build-binary-deb.sh) — see
[`docs/rpi-hil.md`](docs/rpi-hil.md)). GitLab has no release automation set
up (see [`.gitlab-ci.yml`](.gitlab-ci.yml)'s own CI, which is separate from
this).

The one canonical version source is [`dkms.conf`](dkms.conf)'s
`PACKAGE_VERSION` — there's no second manifest file to keep in sync (unlike
some projects that track version in multiple places).

## Cutting a release

Decide the new version (semver: `MAJOR.MINOR.PATCH`, e.g. `0.1.0`), then run
[`scripts/release.sh`](scripts/release.sh):

```sh
scripts/release.sh 0.1.0
# or: RELEASE_VERSION=0.1.0 scripts/release.sh
```

It refuses to run on a dirty working tree, bumps `PACKAGE_VERSION` in
`dkms.conf` to match, commits `Bump version to 0.1.0`, and creates annotated
tag `v0.1.0` — all locally. Nothing is pushed unless you pass `--push` (or
run the `git push` commands it prints at the end yourself); pass `--dry-run`
first if you just want to see what it would do.

Unlike a project with one canonical upstream remote, this repo's tag is
meant to land on every remote that should build a release from it —
`scripts/release.sh` pushes to every configured git remote by default. Use
`--remote NAME` (repeatable) or the `RELEASE_REMOTES` environment variable
(space-separated) to target specific ones instead, e.g. only Gitea while
GitHub is still being set up:

```sh
scripts/release.sh 0.1.0 --remote lnx-gitea --push
```

### Prereleases

A version with a semver pre-release suffix, e.g. `0.1.0-rc0`
(`scripts/release.sh 0.1.0-rc0`), is tagged and built the same way, but both
release workflows detect the `-` suffix and mark the Release as a
prerelease.

## What happens after the tag is pushed

Pushing the tag (alongside the branch, which `scripts/release.sh` also
pushes) triggers two things independently:

* The normal CI workflows (`.gitea/workflows/ci.yml` /
  `.github/workflows/ci.yml`) run on the branch push, same as any other
  commit to `main` — `checkpatch`, the kernel-build matrix, the real
  `dpkg -i`/`dpkg -r` dkms regression test, and so on.
* `.gitea/workflows/release.yml` and `.github/workflows/release.yml` run on
  the tag push. Each one:
  1. Verifies `dkms.conf`'s `PACKAGE_VERSION` matches the tag — fails fast
     with a clear error if they're out of sync (this is exactly what
     `scripts/release.sh` keeps in sync for you, so this should only ever
     fire if a tag was created some other way).
  2. Builds both `.deb` files.
  3. Generates simple release notes from `git log` since the previous tag.
  4. Creates the Release and uploads both files as assets.

These two are deliberately **not** chained together (e.g. via
`workflow_run`) — the release workflows don't re-verify the dkms
install/remove cycle themselves, they rely on the CI workflow triggered by
the same branch push to be the actual correctness gate. If you want to be
sure CI passed before trusting a release's assets, check the CI run for
that commit separately.

## Credentials needed

* **Gitea**: a Gitea access token with repo read/write scope, added as a
  secret named `RELEASE_TOKEN` (the same PAT already used for
  `RENOVATE_TOKEN`, if one exists, works fine here too — no need to
  manage a second one unless you want separate blast radius per
  automation).
* **GitHub**: nothing to create. `.github/workflows/release.yml` uses the
  `GITHUB_TOKEN` every workflow run gets automatically, scoped by the
  `permissions: contents: write` block in that file.

Until the Gitea secret exists, `.gitea/workflows/release.yml` runs and
fails at the release-creation step rather than silently doing nothing —
the build/upload steps before it still succeed and can be inspected.

## What's verified vs. not

Both release workflows' build steps (dkms.conf version check, source `.deb`
build, binary `.deb` build, release-notes generation) were dry-run locally
with `act` simulating a tag-push event before being committed. The final
create-release/upload-asset API calls were checked for correct request
shape against the real endpoints with a deliberately invalid token — both
`api.github.com` and `gitea.h.leenx.nz` accepted the request far enough to
return an auth error rather than a request-parsing error, which confirms
the JSON payloads and URLs are well-formed. Neither could be verified
end-to-end (an actual release created and assets actually attached)
without real credentials against a real repository — the first real tag
push is what confirms that.

`scripts/release.sh` itself was tested more thoroughly, against a scratch
clone: dry-run, a real bump/commit/tag, multi-remote default behaviour,
`--remote` filtering to a single remote, `--push` against a local bare
repo (confirming only the targeted remote received anything), and the
error paths (dirty tree, already-existing tag, malformed version string).
