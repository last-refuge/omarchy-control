# Omarchy package recipe

`omarchy-control/` is the recipe for [Omarchy's package repository](https://github.com/omacom/omarchy-pkgs),
which serves `pkgs.omarchy.org` to every Omarchy machine as the `[omarchy]` pacman repository.
Acceptance there is up to Omarchy's maintainers, so Control is built to be distributed both
ways at once, and every installed copy keeps updating the way it came:

- **From GitHub** (the curl installer, or a package from the releases page): Control's own
  updater keeps using GitHub releases, whatever any repository offers. This is every copy until
  the package is accepted, and the release workflow keeps publishing there regardless.
- **From a package repository** such as `[omarchy]`: the About page says so and offers Omarchy's
  own update, and `omarchy-control update install` declines, so a GitHub build never replaces
  the repository's signed one.

Control tells the two apart from pacman's own records: a repository's copy is verified by its
signature, and a GitHub package is unsigned (`Updater::Source`). Both share the name
`omarchy-control`, so a system update may still move a GitHub copy to a newer repository build;
from then on it is a repository copy and updates that way.

The recipe is kept here, beside the code it builds, and copied into a fork of `omarchy-pkgs`
for the pull request. It follows `pkgbuilds/hype` there, another Qt 6 app built with
`./bin/build`.

## Submit it

1. Tag the release the recipe names (`pkgver`), and let the release workflow publish it.
2. Set `_omarchy_ui_commit` to the tag's `third_party/omarchy-ui` Git revision and refresh the source checksums. The recipe builds from the tag archive and the kit's commit archive.

   ```sh
   cd packaging/omarchy-pkgs/omarchy-control && updpkgsums && rm -rf src *.tar.gz
   ```

3. In a fork of `omacom/omarchy-pkgs`, branched from `master`, copy the recipe to
   `pkgbuilds/omarchy-control/` (both `PKGBUILD` and `.omarchy/package.json`), then check it:

   ```sh
   bash tests/pinned-sources.sh
   python helpers/upstream-watch.py check pkgbuilds/omarchy-control   # expects "already current"
   CONTAINER_ENGINE=podman bin/build --package omarchy-control
   CONTAINER_ENGINE=podman bin/build --package omarchy-control --arch aarch64   # emulated, slow
   ```

   Install the result from `build-output/edge/<arch>/` with `sudo pacman -U` and run through
   `docs/LIVE-CHECKS.md`.
4. Open one pull request against `master` with only `pkgbuilds/omarchy-control/` in it. Say
   what the package is and how it was tested.

A first pull request waits for a maintainer to label it `build-approved`. CI then builds both
architectures, and the pull request merges itself when its checks pass. It is published to
`edge` at once, and to `rc` and `stable` when those are next promoted. Being compiled against
Qt, it cannot use the fast ring.

## After that

The `upstream.watch` in `.omarchy/package.json` follows this repository's GitHub releases
(`vX.Y.Z` tags), resolves the kit's submodule revision from each tag, and opens a pull request for
each update. The pull request merges itself once it builds.
The binary uses no Qt private API (`objdump -T build/omarchy-control | grep
Qt_6_PRIVATE_API` finds nothing), so no `rebuild_on` is needed; check again if that changes.

A change to the packaging itself at the same version bumps `pkgrel` there, in a pull request
to `omarchy-pkgs`.
