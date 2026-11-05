# Bundled local primary model

Every normal Android Debug and Release APK contains the same pinned Qwen3.5 2B
Q4_K_M weight as iOS. The source manifest and unchanged Apache-2.0 license/model
card live in `../../ios/MaiChat/BundledPrimaryModel`; the 1,396,198,496-byte GGUF
is ignored by Git.

Before building a fresh checkout, prepare that shared resource explicitly:

```sh
python MaiChat/ios/MaiChat/scripts/prepare-bundled-primary-model.py --download
```

An existing exact copy can be staged without network access with `--source PATH`.
The fixed SHA-256 is
`57a1085840f497d764a7fc5d346922dbde961efb54cc792ea81d694fd846a1d8`.
Both modes verify the source revision, manifest, weight, license and model card.

Gradle's `stageMaiBundledPrimaryModel` invokes that same preparation script in
local-only packaging mode and stages all five resources under
`assets/MaiLocalModels/Primary`. Missing or corrupt inputs fail the build; there
is no model-free product variant and the running app never downloads the weight.
Python 3 is required; `-PmaichatPythonExecutable=/absolute/path/to/python` selects
a specific interpreter. GGUF entries are stored uncompressed, so a complete APK
is substantially larger than 1.4 GB and the build/install need matching disk space.

`MaiBundledPrimaryModel.resolve(Context)` runs on the existing Controller worker.
It checks the compiled identity against the asset manifest and streams into
`files/MaiLocalModels/Primary`. The full content SHA is included in the installed
filename. A private temporary file is synced and renamed only after its complete
byte count, GGUF header and SHA match. Same-process and OS file locks serialize
installers. A correctly installed file is fully rehashed on every resolution;
an existing different file is preserved rather than replaced while it may be mapped.
Old model versions, imports, account state and conversation files are not removed.
