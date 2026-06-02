# cmosh / PuTTY Mosh Handoff

## Current Goal

Continue stabilizing native PuTTY Mosh and `cmosh`, with emphasis on UDP robustness, input retransmission correctness, terminal redraw behavior, and eventual replacement of raw host-output rendering with a real terminal-state model.

Latest user feedback: maximize/restore works, and the previously failing Lynx/emoji-heavy page now works after receive-side fragment reassembly. User asked to continue and prioritize transport hardening. User also previously reported that sometimes pressing Up for shell history just after login moves the cursor up a line.

## Files Recently Touched

* `cmosh/cmosh_client.c`
* `cmosh/cmosh_client.h`
* `cmosh/cmosh_session.c`
* `cmosh/cmosh_platform.c`
* `cmosh/cmosh_test.c`
* `otherbackends/mosh.c`
* `windows/window.c`
* `AGENTS.md`

## Important Decisions

* Keep the implementation direction: native PuTTY Mosh backend using PuTTY SSH bootstrap, not external `plink`.
* Prefer correctness over speculative local echo or terminal-output heuristics.
* Treat UDP send/recv errors such as buffer exhaustion, connection reset, and host/network unreachable as transient where possible.
* Redraw requests currently resend the current terminal size; this is not yet a real terminal-state replay.
* On Windows, Reset Terminal and maximize/restore paths request a Mosh redraw via `backend_special(..., SS_NOP, 0)`.
* On Mosh UDP timeout, the PuTTY backend attempts to reopen the UDP socket and keeps the old socket if reopen fails. After a successful reopen it immediately sends an ACK on established sessions, or replays the cached association probe before UDP is established.
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
* Empty idle keepalives are now rate-limited to `CMOSH_CLIENT_IDLE_KEEPALIVE_MS` (1s). Retransmits, missing-state diagnostics, and UDP-timeout probes still force an idle packet.
* Client packet constructors avoid mutating send sequence or input retransmission records until packet encoding succeeds. This keeps failed ACK/input/resize/idle packet construction from drifting client state.
* Standalone `cmosh` now ACKs duplicate server packets, matching the PuTTY backend and helping the server recover if our previous ACK was lost.
* PuTTY Mosh `sendbuffer()` and `sendok()` now reflect the cmosh input retransmission queue so paste bursts and lossy links get backend backpressure instead of reporting an empty send buffer while input states are pending.
* PuTTY Mosh now owns accepted terminal input in a backend-side pending buffer until it is successfully packetized into cmosh input states. This prevents large paste bursts from being partially dropped when the retransmission queue fills.
* PuTTY Mosh stores its `Ldisc` pointer and calls `ldisc_check_sendok()` after UDP becomes ready and after received ACKs free input queue capacity.
* Idle/retransmit events now expose input retransmit diagnostics: retransmitted state number, acked/current input state, queued record count, and queued byte count. PuTTY logs these at most every 5 seconds while retransmitting.
* Receive events now expose server `ack_num`/`throwaway_num` plus input queue before/after counters. PuTTY logs throttled Event Log diagnostics when server ACK/throwaway trims queued input, making repeated-character reports easier to correlate with retransmits.
* Standalone `cmosh` treats transient UDP send failures during the established session loop (`would block`, buffer exhaustion, reset/refused, host/network unreachable) as retryable instead of exiting after input/resize state has already entered the retransmission queue.
* If standalone `cmosh` sees a transient UDP send failure for an input, resize, or retransmitted input-state packet, it rolls back that record's send count and makes it immediately eligible for retransmission. This avoids local `ENOBUFS`/would-block delaying the next retry by the normal input retransmit interval.
* Standalone `cmosh` now uses the same transient-aware UDP send helper for the initial association probe and authenticated start ACK, so startup does not abort on local buffer exhaustion or similar retryable send failures.
* Standalone `cmosh` now enters the established session loop from the first authenticated server state if no immediate post-ACK packet arrives. Delayed/lost start ACKs can now be recovered by duplicate server packets and later ACKs instead of falling through and exiting.
* Native PuTTY Mosh now caches the exact initial encrypted association probe and resends those same bytes once per second until the first authenticated UDP packet arrives. Do not regenerate this packet for retries; it uses the same transport nonce.
* Native PuTTY Mosh now marks queued input/resize/retransmit records immediately retryable if a backend UDP send is known not to have been attempted because the socket is unavailable.
* Native PuTTY Mosh now treats UDP plug close/error callbacks as local socket failures to recover from: it attempts a UDP socket reopen before reporting a fatal disconnect. Server shutdown still comes from authenticated Mosh transport state.
* Native PuTTY Mosh now detaches a locally failed UDP socket from the Plug close callback and queues reopen on PuTTY's top-level callback path. If reopen fails because the local network is still unavailable, it keeps the authenticated Mosh state and retries once per second.
* Native PuTTY Mosh `sendok()` now requires an active UDP socket, so PuTTY applies input backpressure during local UDP outages and wakes the line discipline after a successful reopen.
* Native PuTTY Mosh now checks the datagram `sk_write()` result. A nonzero return means PuTTY's UDP layer did not send the datagram, including transient buffer-exhaustion cases; retransmitted input is made immediately retryable instead of waiting for the normal retry interval.
* `cmosh_client_note_idle_send_failed()` rolls back idle/keepalive timing after a failed UDP send, so transient local send failures do not suppress the next keepalive, timeout probe, or retransmit opportunity.

## Protocol Invariants

* Do not retransmit input already acknowledged or thrown away by the server.
* Do not advance local send sequence, input state, or retransmit timestamps for a packet that was not successfully encoded for sending.
* If a packet was encoded but definitely not sent because of a transient local UDP error, queued input must remain owned by the client and should become retryable immediately.
* A missing immediate post-start packet is not fatal after the first authenticated server packet; initialise from that first server state and let normal duplicate/ACK handling converge.
* Repeated Mosh association probes must reuse the exact original encrypted packet bytes, not re-encrypt changed plaintext with the same nonce.
* A local UDP socket close/error is not equivalent to remote exit for Mosh; reopen locally and send a fresh datagram to re-establish the path.
* Reopening a local UDP socket must not run recursively inside the socket close notification. Close/detach the failed socket, queue a top-level reopen, and keep retrying without discarding authenticated transport/input state.
* While the local UDP socket is absent, do not advertise backend send readiness even if the authenticated Mosh session is still logically established.
* A packet is not considered sent merely because it was encoded. If UDP send reports failure, rollback keepalive timing and, for input-bearing retransmits, make the affected input state immediately retryable.
* Once the PuTTY backend accepts terminal input, it must either retain it in `pending_input` or enqueue it in cmosh retransmission state; do not silently drop the tail of an oversized send.
* Server `throwaway_num` is equivalent to an ACK for retransmission purposes: queued input up to that state must be trimmed and must not be retransmitted.
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
* Latest `cmake --build build --target test_cmosh --config Debug` passed after idle/backpressure/non-mutating packet-constructor hardening.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after idle/backpressure/non-mutating packet-constructor hardening.
* Latest `cmake --build build --target putty --config Debug` passed after idle/backpressure/non-mutating packet-constructor hardening.
* Latest `cmake --build build --target putty --config Debug` passed after PuTTY pending-input buffering and retransmit diagnostics.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after retransmit diagnostic fields.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after retransmit diagnostic fields.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after receive ACK/throwaway diagnostics and transient standalone UDP send handling.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after receive ACK/throwaway diagnostics and transient standalone UDP send handling.
* Latest `cmake --build build --target putty --config Debug` passed after receive ACK/throwaway diagnostics and transient standalone UDP send handling.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after transient-send retransmit rollback.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after transient-send retransmit rollback.
* Latest `cmake --build build --target putty --config Debug` passed after PuTTY association probe retries.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after standalone startup-send/post-ACK fallback and PuTTY unsent-packet retry hints.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after standalone startup-send/post-ACK fallback and PuTTY unsent-packet retry hints.
* Latest `cmake --build build --target putty --config Debug` passed after standalone startup-send/post-ACK fallback and PuTTY unsent-packet retry hints.
* Latest `cmake --build build --target putty --config Debug` passed after UDP plug close/error reopen handling and immediate post-reopen datagram send.
* Latest `cmake --build build --target otherbackends --config Debug` passed after queued UDP close/reopen retry hardening.
* Latest `cmake --build build --target putty --config Debug` compiled `mosh.c` but failed to relink because `build\Debug\putty.exe` was open/locked: `LNK1168: cannot open ... putty.exe for writing`.
* Latest `cmake --build build --target otherbackends --config Debug` passed after UDP-outage `sendok()` backpressure.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after UDP datagram send-result handling and idle send-failure rollback.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after UDP datagram send-result handling and idle send-failure rollback.
* Latest `cmake --build build --target putty --config Debug` passed after UDP datagram send-result handling and idle send-failure rollback.

## Known Issues

* Full terminal correctness is not complete; output still depends on raw host-output decoding instead of a full Mosh terminal-state model.
* High-latency or lossy links may still show repeated characters; throwaway handling and resize retransmission are only mitigations.
* Sleep/wake and interface changes should now survive transient local UDP socket close/reopen failures better, but still need live Windows testing with the freshly rebuilt `build\Debug\putty.exe`.
* Up-arrow-after-login issue still needs investigation if it persists after the UTF-8/default rebuild; likely candidates are startup tty modes or local line discipline state before UDP readiness.

## Exact Next Step

Retest the freshly rebuilt `build\Debug\putty.exe` on a high-latency/lossy connection. Focus sleep/wake, local network loss/recovery, paste bursts, startup UDP establishment, and rapid command-history navigation immediately after login. If repeated characters persist, compare Event Log retransmit lines against the new `Mosh server input ACK ...` lines to see whether repeats occur before the server ACK/throwaway transition or after already-trimmed input state.
