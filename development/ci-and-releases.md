# CI and releases

All CI runs in [.github/workflows/build.yml](../.github/workflows/build.yml).

## What runs when

| Trigger | Jobs |
| --- | --- |
| Pull request to `main` | Transport tests on Windows, macOS, and Linux; a ThreadSanitizer build of them on Linux; Linux and Windows app builds with the media/UI tests. No packaging. |
| Push to `main`, `v*` tag, manual run | Everything above plus the macOS app build, packaging on all three platforms, and a smoke test that launches each packaged app and renders a demo screenshot. |
| Other branch pushes | Nothing. |

A newer push to a pull request cancels its superseded run; runs on `main` are never cancelled.

All app builds wait for the core tests on every platform. A core failure cancels the remaining core matrix jobs and skips the app builds. The final `CI` job fails if any required build/test job (including ThreadSanitizer) fails, is cancelled, or is unexpectedly skipped; the macOS app job is intentionally skipped on pull requests.

Workflow actions are pinned to commit SHAs, runners to explicit images, RtAudio to its 6.0.1 commit, and the linuxdeploy tools to tagged releases verified by SHA-256.

Packages contain the app, its runtime libraries, `packaging/README.txt`, the repository `README.md`, `LICENSE`, third-party notices collected by [packaging/collect_licenses.py](../packaging/collect_licenses.py), and the whole `docs/` folder (the user manual).

## Releasing

1. Bump the version in `CMakeLists.txt`, `vcpkg.json`, and the first line of `packaging/README.txt`.
2. Replace `packaging/release-notes.md` with the notes for the new version. Link to files at the release tag (`.../blob/vX.Y.Z/...`) so the links stay stable.
3. Merge that to `main`, then tag the merge commit and push the tag:

   ```sh
   git tag v0.3.0 <merge commit> && git push origin v0.3.0
   ```

The app's `--version` output and macOS `CFBundleShortVersionString` / `CFBundleVersion` are generated from the CMake project version. CI verifies both fields in the packaged macOS app before archiving it.

The tag runs the full build. After every build and test passes, the `release` job:

- checks the tag against the project version (`vX.Y.Z`, or `vX.Y.Z-suffix` for a prerelease of it) and the version named in `packaging/README.txt`
- publishes the macOS, Windows, and Linux packages with a source archive, `SHA256SUMS.txt`, and the release notes

0.x versions and suffixed tags are marked as prereleases. Re-running the job for an existing release replaces its assets and notes.

Code signing and notarization are not configured; macOS packages are signed ad-hoc.

## Merge requirements

The repository ruleset in [.github/main-ruleset.json](../.github/main-ruleset.json) requires pull requests to `main`, an up-to-date branch, and a passing `CI` check from GitHub Actions, with no bypass actors. Committing the JSON alone does not enforce anything; a repository administrator applies it under **Settings → Rules → Rulesets → Import a ruleset**, or with:

```sh
gh api --method POST repos/staubichsauger/simpleAVshow/rulesets --input .github/main-ruleset.json
```

The `CI` check must have reported at least once before it can be required. If the ruleset already exists, update it rather than creating another copy.
