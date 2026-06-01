# cmosh / PuTTY Mosh Handoff

## Current Goal

Continue stabilizing native PuTTY Mosh and `cmosh`, with emphasis on UDP robustness, input retransmission correctness, terminal redraw behavior, and eventual replacement of raw host-output rendering with a real terminal-state model.

Latest user feedback: maximize/restore works, and the previously failing Lynx/emoji-heavy page now works after receive-side fragment reassembly. User asked to continue and prioritize transport hardening. User also previously reported that sometimes pressing Up for shell history just after login moves the cursor up a line.

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
* Host protobuf decoding now skips unknown varint, length-delimited, fixed32, and fixed64 fields instead of dropping the whole server update. This avoids treating richer display/emoji-related fields as bad packets.
* PuTTY Mosh logs a one-shot Event Log diagnostic if a server update decodes successfully but contains no raw host-output bytes, which is a strong signal that the current raw-output shim needs the real terminal-state renderer for that screen.
* Upstream `hostinput.proto` confirms server diffs are still `HostMessage.instruction = 1`, extension `HostBytes = 2`, `hoststring = 4`; the Lynx emoji freeze is more likely PuTTY interpreting UTF-8 emoji bytes as 8-bit C1 terminal controls than a missing protobuf display message.
* Windows PuTTY now sets `CONF_line_codepage` to `UTF-8` by default for Mosh sessions before fonts and terminal unicode tables are initialised, but only if the user did not explicitly configure a line codepage.
* Receive-side Mosh transport now reassembles multi-fragment server instructions before zlib/protobuf decode. Previously only single final fragments decoded, so larger screen frames could be treated as bad packets, leaving the old screen visible while input continued.
* Fragment reassembly is held in `cmosh_transport_state`; `cmosh_client_recv_packet` now returns `CMOSH_CLIENT_RECV_PENDING` while waiting for remaining fragments and does not ACK a server state until the full instruction is decoded/applied.
* Fragment assembly is now cleared if a malformed/conflicting fragment arrives or if a complete single-fragment packet arrives while an old partial assembly is pending. This avoids stale fragment state poisoning later receives.
* `cmosh_client_make_input` now validates/encodes input before appending it to the retransmission queue, and rejects chunks larger than `CMOSH_CLIENT_INPUT_CHUNK_MAX`. PuTTY Mosh splits larger backend sends into protocol-sized input packets.

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
* Latest `cmake --build build --target test_cmosh --config Debug` passed after protobuf unknown-field tolerance.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after protobuf unknown-field tolerance.
* Latest `cmake --build build --target putty --config Debug` compiled but failed to relink because `build\Debug\putty.exe` was open/locked: `LNK1168: cannot open ... putty.exe for writing`.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after the Windows Mosh UTF-8 default change.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after the Windows Mosh UTF-8 default change.
* Latest `cmake --build build --target putty --config Debug` compiled after the Windows Mosh UTF-8 default change, but failed to relink because `build\Debug\putty.exe` was open/locked: `LNK1168: cannot open ... putty.exe for writing`.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after receive-side transport fragment reassembly.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after receive-side transport fragment reassembly.
* Latest `cmake --build build --target putty --config Debug` passed after receive-side transport fragment reassembly.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after fragment-state cleanup and input chunk hardening.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after fragment-state cleanup and input chunk hardening.
* Latest `cmake --build build --target putty --config Debug` passed after fragment-state cleanup and input chunk hardening.

## Known Issues

* Full terminal correctness is not complete; output still depends on raw host-output decoding instead of a full Mosh terminal-state model.
* High-latency or lossy links may still show repeated characters; throwaway handling and resize retransmission are only mitigations.
* Sleep/wake and interface changes may still need deeper UDP reopen or retransmission timer work.
* Up-arrow-after-login issue still needs investigation if it persists after the UTF-8/default rebuild; likely candidates are startup tty modes or local line discipline state before UDP readiness.

## Exact Next Step

Retest with `build\Debug\putty.exe` on a high-latency/lossy connection, especially paste bursts and rapid command-history navigation immediately after login. If repeated characters persist, inspect the server ACK/throwaway progression and retransmission timing next.
