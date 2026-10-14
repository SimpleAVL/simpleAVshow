# CI and releases

All CI runs in [.github/workflows/build.yml](../.github/workflows/build.yml).

## What runs when

| Trigger | Jobs |
| --- | --- |
| Pull request to `main` with code/build changes | Transport tests on Windows, macOS, and Linux; a ThreadSanitizer build of them on Linux; Linux and Windows app builds with the media/UI tests. No packaging. |
| Push to `main` with code/build changes | Everything above plus the macOS app build and tests. No packaging. |
| `v*` tag or manual run | Full build/tests on all platforms, packaging, and a smoke test that launches each packaged app and renders a demo screenshot. Tags also publish a GitHub release. |
| Documentation-only PR or push to `main` | Change classification and the required `CI` gate; native jobs are intentionally skipped. |
| Other branch pushes | Nothing. |

A newer push to a pull request or `main` cancels its superseded run. Tags and manual runs use separate concurrency groups and are not cancelled by newer pushes.

The change classifier compares the PR head against its merge base, or a `main` push against the previous commit. It skips native jobs only when every changed path is `README.md`, `CONTRIBUTING.md`, Markdown under `development/`, or Markdown/images under `docs/`. Unknown paths, source/build/workflow changes, and unavailable comparisons run the native checks. Renames check both the old and new paths. Tags and manual runs always build, even for documentation-only changes. The workflow itself is never filtered out, so the required `CI` check still reports a result.

All app builds wait for the core tests on every platform. A core failure cancels the remaining core matrix jobs and skips the app builds. The final `CI` job fails if any required build/test job (including ThreadSanitizer) fails, is cancelled, or is unexpectedly skipped; the macOS app job is intentionally skipped on pull requests. For documentation-only changes, the gate requires successful classification and verifies that all native jobs were skipped. Classification failures or unexpected job results fail the gate.

Workflow actions are pinned to commit SHAs, runners to explicit images, RtAudio to its 6.0.1 commit, and the linuxdeploy tools to tagged releases verified by SHA-256.

Core and ThreadSanitizer jobs have 10-minute limits; Linux/macOS app jobs have 30-minute limits and Windows has 45 minutes to allow for cold dependency builds. Classification and the CI gate have 5-minute limits; publishing has 10 minutes. CTest limits each core test executable to 120 seconds and each app/MCP test executable to 300 seconds.

Packaging runs only on version tags or manual runs. Actions packages and preview screenshots expire after **one day**; download manual build artifacts promptly. Published GitHub Release assets remain available independently of these temporary artifacts. To rerun a release after its Actions artifacts expire, rerun the full workflow so the packages are rebuilt.

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
