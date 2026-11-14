# Paid image generation pause

## Reproduction and expected behavior

Request a new image, an image edit/reference generation, or a supported paid revision through `wan_image`, `kling_bailian_image`, `minimax_image`, `glm_image` or `seedream_image`. All five must be absent from public model tool lists. A retained/direct tool instance must return the existing temporarily-disabled error before credential retrieval, upload or provider HTTP. This pause must not hide local image viewing, cropping or conversion, or unrelated video tools.

The user requested this cost pause after the earlier Wan video pause. This change retains image provider implementations and supported recovery/cancel/delete paths for existing paid tasks. Seedream was already internal; its ordinary factory still needs the execution guard. The MiniMax guard applies only to its image instance.

## Implementation

Each image entry uses the common disabled-result contract: temporarily disabled, trusted not-started state and no submitted task. The desktop and mobile model registrations retain internal instances and remove public image generation entries. Both prompts state: `Paid image generation is temporarily disabled to control costs.` Later retry guidance cannot override the pause.

The underlying private provider implementations remain intact. Existing task recovery remains internal and keeps its actual supported operations; no synthetic success, canceled task, or replacement provider is returned. Local free processing and the independently controlled video entries keep their existing behavior.

## Verification and scope

Windows 11, normal desktop CMake cache, MSVC 14.44. The four affected offline provider suites (Creative, ModelStudio, GLM and Ark) compiled, linked and ran successfully against the normal `maiagent` archive. This was repeated in the broader 2026-11-15 Core consumer run alongside ordinary file/tool tests. Desktop actual model-request fixture passed with the five paid images absent and existing non-image controls present. Tests remove live/diagnostic/recovery opt-ins and use owned files/fake clients or loopback; no charged provider call or real SDK send was made.

Mac separately consumed the precise 15-file Core/mobile image slice, checked previous/new blobs and canonical hashes, and reported eight ordinary Core suites passing. Its mobile actual-request selector also passed: Wan video and all five image entries absent; Kling video, `view_image`, and `mobile_transform_image` present, with the fixed pause sentence asserted. Incoming receipt `144115251141768230-1794675069-96923055.json` contains one build and eight compile/link/run groups, all 25 exits zero, for arm64 macOS. It contains no Windows or Android physical acceptance claim.

Evidence is retained under `.cache/paid-image-pause-20261114/` and `.cache/im-delivery-review-20261114/`. The image-only source projections exclude separate binding protocol and `agent_send_file` work. The already installed iPhone build 211 contains only the earlier Wan video pause; it is not evidence of this image change. No release, installation or running user process was changed for these tests.
