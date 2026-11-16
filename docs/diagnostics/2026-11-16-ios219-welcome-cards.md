# iOS welcome cards, build 219

The final change removes only the QA, image-creation and video-creation introduction
rows. Ordinary chat, the composer/voice input, models and tool registration remain
unchanged. The earlier QA-only 218 source candidate was never applied by Root.

Root reviewed and applied the final 2,067-byte patch once, verifying its SHA-256
`e9a38fd8ac933aee30a374740347945e86b2aa8070da35634735afb6bd9da114`, both original
Git blobs and both final blobs. AIAssistantView is now
`2774fb5c9971a222325d082eae04452f04a290dc`; AIAssistantUITests is
`06c9abb662d03ad1eb87ec56e2194fc0c211fa1b`, preserving the previously integrated
217 navigation tests.

The Mac collaborator ran the ordinary Simulator welcome driver: 1/0 in 8.240
seconds. The three labels were absent, the composer existed, remaining tools still
scrolled, and top/bottom screenshots were inspected. Normal 2.2.4(219) device build
passed in 140.364 seconds, signature passed, keep-data install passed in 12.492
seconds, and a normal launch without fixture arguments reported PID 67679 and fresh
installed metadata 2.2.4(219). Candidate executable SHA-256:
`8bff1ed4d33e85cd0b3b15d948d7563e8ea70478ba45d7b5ef4229d42e10f773`.
Device binary readback and full real-chat gesture acceptance were not claimed.
