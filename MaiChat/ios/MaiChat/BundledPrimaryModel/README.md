# Bundled local primary model

The normal iOS App includes the Qwen3.5 2B Q4_K_M GGUF quantized by bartowski in both
Debug and Release. The primary Agent uses its text input path and non-thinking profile; no vision projector is included. Its source revision, byte count and
SHA-256 are fixed in `manifest.json`, `SOURCE.json`, the preparation script and
`AILocalModelStore`; changing the model requires updating these together.

The 1,396,198,496-byte weight is ignored by Git. Prepare a checkout before building:

```sh
python3 MaiChat/ios/MaiChat/scripts/prepare-bundled-primary-model.py --download
```

An existing copy of the exact public weight can be staged without network access:

```sh
python3 MaiChat/ios/MaiChat/scripts/prepare-bundled-primary-model.py --source /absolute/path/Qwen_Qwen3.5-2B-Q4_K_M.gguf
```

The default and `--check` only verify local inputs; neither downloads. A missing,
changed or symbolic-link input fails with an actionable error. Explicit staging
also preserves an existing invalid destination for inspection.

The normal Xcode resource phase calls:

```sh
python3 "$PROJECT_DIR/scripts/prepare-bundled-primary-model.py" \
  --bundle-directory "$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/MaiLocalModels/Primary"
```

It checks the exact weight and source attribution, then copies `manifest.json`,
the GGUF, `LICENSE`, `SOURCE.json` and `UPSTREAM_MODEL_CARD.md` into the App.
This phase is independent of the C++ build and has no configuration condition.
Builds fail when preparation is incomplete; they never silently omit the model
or contact a model service. Repeated packaging reuses verified identical output. After a verified replacement, the build phase retires only the exact former bundled GGUF from incremental App resources; it preserves unrelated files and every runtime Documents file.

On first use `AILocalModelStore.bundledPrimarySelection()` checks the resource
manifest against the compiled identity and streams the weight into the App's
Documents/MaiLocalModels directory while hashing it. The destination name contains
the full content SHA; an existing different file is never overwritten or removed.
Concurrent installers can only reuse a correctly verified completed copy. This
keeps the file within the Agent sandbox and preserves existing imports and files
that the shared runtime may already map. Later resolutions verify the installed
SHA again. This method does not depend on import catalog state or an import UI.

The upstream license and model card are preserved without changes. `SOURCE.json`
records their hashes and the exact source URLs; the App ships these notices next
to the weights. The tiny `stories260K` Debug engine fixture remains separate and
is never the normal local primary model.
