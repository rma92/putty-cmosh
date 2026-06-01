# cmosh / PuTTY Mosh Handoff

## Current Goal

Continue stabilizing native PuTTY Mosh and `cmosh`, with emphasis on UDP robustness, input retransmission correctness, terminal redraw behavior, and eventual replacement of raw host-output rendering with a real terminal-state model.

Latest user feedback: maximize/restore now works. Lynx on emoji-heavy/forum pages can drive very high data usage and become unresponsive, likely from large unicode/full-screen updates causing decode/ACK failure loops.

## Files Recently Touched

* `cmosh/cmosh_client.c`
* `cmosh/cmosh_client.h`
* `cmosh/cmosh_session.c`
* `cmosh/cmosh_session.h`
* `cmosh/cmosh_platform.c`
* `cmosh/cmosh_proto.c`
* `cmosh/cmosh_proto.h`
* `cmosh/cmosh_test.c`
* `cmosh/cmosh_transport.c`
* `otherbackends/mosh.c`
* `windows/window.c`
* `AGENTS.md`

## Important Decisions

* Keep the implementation direction: native PuTTY Mosh backend using PuTTY SSH bootstrap, not external `plink`.
* Prefer correctness over speculative local echo or terminal-output heuristics.
* Treat UDP send/recv errors such as buffer exhaustion, connection reset, and host/network unreachable as transient where possible.
* Redraw requests currently resend the current terminal size; this is not yet a real terminal-state replay.
* On Windows, Reset Terminal and maximize/restore paths request a Mosh redraw via `backend_special(..., SS_NOP, 0)`.
* On Mosh UDP timeout, the PuTTY backend attempts to reopen the UDP socket and keeps the old socket if reopen fails.
* Resize updates are now recorded in the client input/retransmission queue as encoded diffs, so lost maximize/restore resize packets can be retransmitted before later input states.
* Windows maximize/restore now uses the same terminal-resize predicate as ordinary WM_SIZE resize events: `RESIZE_TERM` or `RESIZE_EITHER` with Alt not pressed. Previously maximize/restore only called `wm_size_resize_term()` for `RESIZE_TERM`, so sessions using the default "resize either" behavior could redraw locally without sending the remote pty resize to Mosh/vim.
* Host output decoding now has a callback/streaming API, so large unicode/full-screen server updates no longer have to fit in an 8 KiB temporary output buffer before being ACKed.
* Server diffs can now be up to 64 KiB and out-of-order queued diffs are allocated dynamically, avoiding large inline `cmosh_client` structs while still tolerating larger compressed server updates.

## Protocol Invariants

* Do not retransmit input already acknowledged or thrown away by the server.
* Keep server sequence replay, out-of-order fragments, missing state gaps, and input retransmission state separate.
* Do not trust network input; validate packet lengths and state transitions before use.
* Preserve behavior of non-Mosh PuTTY backends.

## Verification Already Run

* `cmake --build build --target test_cmosh --config Debug` passed.
* `.\build\cmosh\Debug\test_cmosh.exe` passed.
* `cmake --build build --target test_conf --config Debug` passed.
* `.\build\Debug\test_conf.exe` passed.
* Latest `cmake --build build --target putty --config Debug` passed after making resize retransmittable.
* Latest `cmake --build build --target putty --config Debug` passed after routing maximize/restore through the normal terminal resize predicate.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after streaming host-output/dynamic server-diff changes.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed.
* Latest `cmake --build build --target putty --config Debug` passed after streaming host-output/dynamic server-diff changes.

## Known Issues

* Full terminal correctness is not complete; output still depends on raw host-output decoding instead of a full Mosh terminal-state model.
* High-latency or lossy links may still show repeated characters; throwaway handling and resize retransmission are only mitigations.
* Sleep/wake and interface changes may still need deeper UDP reopen or retransmission timer work.
* Lynx/emoji-heavy pages need user retesting with `build\Debug\putty.exe` after the large-output ACK-loop mitigation.

## Exact Next Step

Test `.\build\Debug\putty.exe` with Lynx on the emoji-heavy/forum page that previously hung. If it still spins, add short diagnostics for bad packet/decode failures and ACK advancement in `mosh_udp_receive()`/`cmosh_client_process_packet()`, then continue toward the real terminal-state model and lossy-link retransmission fixes.
