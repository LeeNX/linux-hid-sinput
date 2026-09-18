# Release process

Releases are git tags, plus two `.deb` files: the source DKMS package
(`sinput-dkms_<version>_all.deb`, from
[`scripts/build-source-deb.sh`](scripts/build-source-deb.sh)) and the
precompiled Raspberry Pi 3 binary package
(`sinput-modules-<kernel-release>_<version>_arm64.deb`, from
[`scripts/build-binary-deb.sh`](scripts/build-binary-deb.sh) — see
[`docs/rpi-hil.md`](docs/rpi-hil.md)). How those two files actually reach a
release differs by platform (see "What happens after the tag is pushed"
below) — GitHub automates it fully, Gitea builds the files but stops short
of publishing them for a network reason specific to that runner, and GitLab
has no release automation at all (see [`.gitlab-ci.yml`](.gitlab-ci.yml)'s
own CI, which is separate from this).

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
`dkms.conf` to match (or, if `dkms.conf` is already at that version — as it
was for the very first release — skips straight to tagging `HEAD` with no
bump commit), and creates annotated tag `v<version>` — all locally. Nothing
is pushed unless you pass `--push` (or run the `git push` commands it
prints at the end yourself); pass `--dry-run` first if you just want to see
what it would do.

Unlike a project with one canonical upstream remote, this repo's tag is
meant to land on every remote — `scripts/release.sh` pushes to every
configured git remote by default. Use `--remote NAME` (repeatable) or the
`RELEASE_REMOTES` environment variable (space-separated) to target specific
ones instead:

```sh
scripts/release.sh 0.1.0 --remote lnx-gitea --push
```

Note that `scripts/release.sh` only pushes `main` and the tag — if a remote
is otherwise behind (e.g. you've been pushing to one remote all along and
only just added another), push `main` there first so the tag lands on a
commit history that remote actually has.

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
  the tag push. Both verify `dkms.conf`'s `PACKAGE_VERSION` matches the tag
  (fails fast if a tag was created some other way) and build both `.deb`
  files, then diverge:
  * **GitHub** generates release notes from `git log` since the previous
    tag, creates the Release via the REST API (using the auto-issued
    `GITHUB_TOKEN`, no secret to create), and uploads both files as assets.
  * **Gitea** uploads both `.deb` files plus the generated release notes as
    a plain downloadable workflow artifact instead of creating the Release
    automatically. This Gitea instance's job containers can reach the
    public internet (they install the Raspberry Pi apt archive over HTTPS
    a few steps earlier in the same job) but not `gitea.h.leenx.nz` itself
    — almost certainly because that hostname only resolves via internal
    LAN DNS that the job's container network doesn't have access to. That's
    a runner/container-network configuration question on the host running
    act_runner, not something fixable from workflow YAML alone. Until/unless
    that's addressed, creating the actual Gitea Release is a manual step:
    download the artifact from the workflow run, pick the release from the
    already-pushed tag in the Gitea web UI, paste in `release-notes.md`,
    and attach the two `.deb` files.

These two workflows are deliberately **not** chained to the main CI
workflow (e.g. via `workflow_run`) — they don't re-verify the dkms
install/remove cycle themselves, they rely on the CI workflow triggered by
the same branch push to be the actual correctness gate. If you want to be
sure CI passed before trusting a release's assets, check the CI run for
that commit separately.

## Credentials needed

* **Gitea**: none currently, since release creation there is a manual step
  (see above). If the container network issue gets fixed and
  `.gitea/workflows/release.yml` goes back to calling the release/asset API
  directly, it would need a Gitea access token with repo read/write scope
  as a secret named `RELEASE_TOKEN` (the same PAT already used for
  `RENOVATE_TOKEN`, if one exists, would work fine there too).
* **GitHub**: nothing to create. `.github/workflows/release.yml` uses the
  `GITHUB_TOKEN` every workflow run gets automatically, scoped by the
  `permissions: contents: write` block in that file.

## What went wrong on the first real release, and what's still unverified

Cutting `v0.1.0` was the first time either release workflow ran against a
real tag push, and it surfaced two things `act` and standalone request
checks hadn't caught:

* **Gitea**: the container-network DNS issue described above. Fixed by
  switching that workflow to produce a downloadable artifact instead of
  calling the release API (see above) — not a real fix for the underlying
  network limitation, just a way to route around it.
* **GitHub**: raw `git` commands in the "Generate release notes" step
  failed with `fatal: detected dubious ownership in repository` — the
  checkout is owned by a different UID than the one running later `run:`
  steps in GitHub's hosted container setup. `act` doesn't reproduce this
  (its container UID handling differs from a real GitHub-hosted runner),
  so no amount of local dry-running caught it. Fixed by explicitly running
  `git config --global --add safe.directory "$GITHUB_WORKSPACE"` right
  after checkout.

Both fixes were re-verified with `act` before being committed, though `act`
couldn't have caught the dubious-ownership bug in the first place, so that
fix's real confirmation is the next tag push. The GitHub release/asset API
calls remain unconfirmed end-to-end: the `v0.1.0` run failed at the
release-notes step, before ever reaching them, so no GitHub Release exists
yet for `v0.1.0` — pick it manually from the tag for now, the same as
Gitea, or push a new tag once you're ready to trust the fixed workflow.

`scripts/release.sh` itself was tested more thoroughly before ever running
for real, against a scratch clone: dry-run, a real bump/commit/tag,
multi-remote default behaviour, `--remote` filtering to a single remote,
`--push` against a local bare repo (confirming only the targeted remote
received anything), the "already at target version" first-release case,
and the error paths (dirty tree, already-existing tag, malformed version
string).
