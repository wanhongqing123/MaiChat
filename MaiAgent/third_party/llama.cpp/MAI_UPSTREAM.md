# llama.cpp, ggml and mtmd source pin

- Upstream: https://github.com/ggml-org/llama.cpp
- Tag: `b12065`
- Commit: `ee17f293b638295651c6ff4752a4dce102c6b8e4`
- Git tree: `625866e031a0127781b4805a4455f10ff3f50647`
- Source archive: https://codeload.github.com/ggml-org/llama.cpp/tar.gz/ee17f293b638295651c6ff4752a4dce102c6b8e4
- Archive SHA-256: `ccae5be5b19211072aea391a726c80f0848e191211d121228e6c71b9f4ab3bd8`
- Archive size: 38,910,289 bytes; upstream regular files: 4,021 / 182,799,898 bytes.
- Main license: MIT, see `LICENSE`. All upstream source and license notices are retained.
- The complete original source has no local upstream-code patches. `MAI_*` files are local provenance records.

`MAI_SOURCE_MANIFEST.sha256` lists every original upstream file; its SHA-256 is
`c37eadaa3aae77a4655e7c670ab65775b84fe33369217a96304869a021fd0953`.
Every original file was also checked against the fixed commit's Git blob ID before import.
`MAI_SOURCE_AUDIT.json` records the source checks and all 19 GGUF vocabulary fixtures.
Each fixture has zero tensors; no inference weights or prebuilt SDKs are included.
No original file exceeds 40 MiB and no PEM private-key header was found. The upstream
`docs/development/llama-star/idea-arch.key` is a Keynote ZIP document, not key material.

## Library build

`../MaiLlamaDependencies.cmake` provides `mai_add_llama_dependencies()` and the
`mai_llama` interface target. It builds local static `llama`, `mtmd` and ggml targets,
without FetchContent, package downloads, system ggml or a prebuilt XCFramework.
Consumers inherit `include/`, `ggml/include/` and `tools/mtmd/` through target usage.
The runtime's private use of `stb/stb_image.h` should add this tree's `vendor/`
directory privately, without defining a second `STB_IMAGE_IMPLEMENTATION`.

`LLAMA_BUILD_COMMON`, tools, examples, tests, server, app, Web UI, OpenSSL and
subprocess support are disabled; `LLAMA_BUILD_MTMD` is enabled. `GGML_BACKEND_DL`
is off, so the engine does not scan external backend libraries. CPU remains built.
Apple adds static Metal and Accelerate backends. `GGML_METAL_EMBED_LIBRARY=ON`
places shader source in the archive; no runtime shader source path is required.
`GGML_NATIVE=OFF` avoids compiling one developer machine's native CPU selection.
The wrapper pins build-identification values rather than reporting MaiChat's enclosing
Git commit as the ggml source version. Its local variables do not change parent options.
For iOS deployments before 16.4, the wrapper undefines upstream's new-LAPACK/ILP64
selection on ggml CPU/BLAS targets and uses Accelerate's established LP64 CBLAS ABI.
This avoids importing the iOS-16.4-only `cblas_sgemm$NEWLAPACK$ILP64` symbol while
retaining Accelerate, CPU and Metal; unguarded availability warnings are errors there.
Android `armeabi-v7a` disables only `GGML_LLAMAFILE`: its optional SGEMM calls
FP16 NEON load intrinsics unavailable on ARMv7. The ordinary ggml ARM/NEON CPU
backend remains enabled, and the existing 32-bit ABI is retained. Other ABIs keep
their original llamafile/backend selection.

The reproducible library/link check is:

```sh
MaiAgent/scripts/build-llama-apple.sh macos /private/tmp/mai-llama-macos
MaiAgent/scripts/build-llama-apple.sh ios-device /private/tmp/mai-llama-ios-device
MaiAgent/scripts/build-llama-apple.sh ios-sim /private/tmp/mai-llama-ios-sim
MaiAgent/scripts/build-llama-android.sh armeabi-v7a /private/tmp/mai-llama-android32 "$ANDROID_NDK_HOME"
```

The script defaults to two build jobs (`MAI_LLAMA_BUILD_JOBS` overrides this).
The iOS device/simulator checks use the product's iOS 16.0 deployment target;
this wrapper does not raise the application's minimum OS version.
Build outputs stay in the selected directory. macOS runs `MaiLlamaBuildSmoke` against
the actual static archives; iOS links the same no-model smoke for the target SDK.
The smoke checks public llama/mtmd defaults and backend initialization/free, not model
quality or device inference. Runtime/model tests remain separate.

Static archives are `libllama.a`, `libmtmd.a`, `libggml.a`, `libggml-base.a`,
`libggml-cpu.a`, `libggml-blas.a`, `libggml-metal.a` and `libvendor-hash.a`.
Android has the same dependency closure without the two Apple BLAS/Metal archives.
Header-only miniaudio/stb/sheredom usage is carried by the mtmd target. Apple final
links need Foundation, Metal, MetalKit and Accelerate; Threads usage is also propagated.
Link `mai_llama` in CMake instead of reconstructing this closure per platform.

## Video and command boundary

This is a source-library integration, not an embedded llama CLI. The upstream CLI
has not been adapted for repeated in-process calls, process exits, signals, global
state and bounded output under `MaiAgent/docs/EmbeddedCommandLineTools.md`; it is
not exposed as an Agent command by this dependency module.

Upstream `MTMD_VIDEO` in this revision runs external `ffprobe` and `ffmpeg` through
`subprocess_create`, searches PATH, inherits the environment and reads a rawvideo pipe.
That adapter is disabled on all MaiChat platforms because the App must not launch
external programs. This does not remove the product's source-built in-process FFmpeg.
The intended shared route is the existing FFmpeg extraction API -> bounded RGB frames
and timestamps -> mtmd bitmaps/chunks. A final video-input API and its end-to-end tests
are not provided by this build module; do not advertise them as implemented.

Model GGUF architecture, matching projector, tokenizer/chat format and context budgets
are runtime responsibilities. `llama_context` and `mtmd_context` must both be released
before their borrowed model is freed. Weight quantization does not set KV precision.

## Bundled dependency notices

Keep the upstream `licenses/` tree and license blocks/files in `vendor/` and `ggml/`.
Compiled helper dependencies include xxHash (BSD-2-Clause, `vendor/hash/xxhash/LICENSE`),
SHA256 (public domain, `vendor/hash/sha256/LICENSE`), rotate-bits (MIT), and the license
blocks in miniaudio, stb_image and sheredom. Product redistribution must carry these
notices along with the main MIT notice. Unused upstream UI/server/example dependencies
remain in the complete source tree but are not enabled by the library-only module.
`MAI_THIRD_PARTY_NOTICES.txt` collects these source license blocks, plus the Mozilla
llamafile CPU-kernel MIT notice, as one small text resource for App packaging.
Keep this resource synchronized with the pin and enabled backends.
