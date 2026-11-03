# TinyStories local engine test fixture

This is a 1,185,376-byte English text generation test, not a chat, OCR or VLM model.
It is bundled in **Debug only**, copied without replacement to the app Documents/MaiLocalModels
directory, and used by the ordinary local-inference screen and the shared `llama` tool.
Release includes the user GGUF importer, but does not bundle this CI weight.

The original [karpathy/tinyllamas model card](https://huggingface.co/karpathy/tinyllamas/blob/0bd21da7698eaf29a0d7de3992de8a46ef624add/README.md)
labels the model MIT. `LICENSE` preserves the upstream llama2.c MIT notice. The pinned
GGUF conversion comes from ggml-org's CI model repository. Its upstream README says:

> Various models to be used in llama.cpp CI workflow.
>
> Do not use it in production.

That warning is retained in `UPSTREAM_CI_README.md`; it is not presented as a production
model. `SOURCE.json` records the exact artifact, original model and document hashes.
No model is downloaded by the app or its build script.

SHA-256: `270cba1bd5109f42d03350f60406024560464db173c0e387d91f0426d3bd256d`

Use raw completion (no chat template), context 256, at most 32 output tokens, prompt
`Once upon a time, a little bird`, and temperature 0. English output can be repetitive;
the purpose is checking real GGUF loading, token generation, cancellation and unloading.
It does not establish quality or memory support for any larger model.
