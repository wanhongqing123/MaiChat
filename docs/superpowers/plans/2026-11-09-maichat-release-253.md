# MaiChat 2.2.53 desktop release

## Authorized scope

The user authorized new Windows and Mac packages and publication to GitHub
Release after the Qt 6 validation. Root owns the shared desktop version source,
Windows artifacts, tag, draft and final publication. The Mac peer owns only its
Mac artifacts and uploads them to Root's draft. Existing releases/assets and
installed applications are preserved. SVG remains a UI dependency.

The current public release is v2.2.52. The new desktop version is 2.2.53, read
from desktop/CMakeLists.txt by both packaging paths. Baseline functional source
is 8b717c48e137c4548811d9addbe36e5dc9c4cc9f. Its Windows 17/17 and Mac 18/18
affected suites, owned UI evidence and native 48-case tests remain applicable;
the release changes only version metadata.

## Release procedure and expected result

- [ ] Commit/push the shared version and freeze one source SHA for both hosts.
- [ ] Normally configure/build both hosts. Verify actual version 2.2.53.
- [ ] Windows: create a fresh portable stage/ZIP and a separate installer stage,
  verify PE closure and model hashes, ZIP CRC/extraction and all payload hashes,
  then normal/debug bare-PATH smoke both stage and extracted payloads.
  Compile NSIS and extract its actual payload for comparison; do not run the
  installer against Program Files or terminate an existing application.
- [ ] Mac: create stage, DMG and ZIP; verify version, signature, dependency/model
  closure and normal/debug sandbox smoke from stage, readonly DMG and actual ZIP.
  Record Apple Silicon/macOS 27.2 execution, without a macOS 14.4 claim.
- [ ] Prepare user-facing release notes, per-host manifests and SHA256SUMS. State
  unsigned Windows installer and Mac ad-hoc/not-notarized boundary accurately.
- [ ] Root creates one new tag/draft at the frozen source commit. Each host adds
  only its four new assets; never clobber previous assets. Compare GitHub asset
  bytes/digests and the downloaded checksum files with local verified artifacts.
- [ ] Publish the complete eight-asset release, check public latest/tag/source
  identity, and preserve the previous release's assets unchanged.

## Verification boundaries

This is a desktop client release. It does not deploy SessionSync, run live model
requests, replace an installed App, operate a real camera or inject desktop
input. Those earlier acceptance boundaries remain explicit in the release notes.
