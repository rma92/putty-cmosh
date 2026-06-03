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
* `cmosh/cmosh_base64.c`
* `cmosh/cmosh_fragment.c`
* `cmosh/cmosh_ocb.c`
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
* On Mosh UDP timeout, the PuTTY backend attempts to reopen the UDP socket and keeps the old socket if reopen fails. After a successful reopen it immediately sends an ACK on established sessions, or replays the cached association probe before UDP is established.
* Resize updates are now recorded in the client input/retransmission queue as encoded diffs, so lost maximize/restore resize packets can be retransmitted before later input states.
* Windows maximize/restore now uses the same terminal-resize predicate as ordinary WM_SIZE resize events: `RESIZE_TERM` or `RESIZE_EITHER` with Alt not pressed. Previously maximize/restore only called `wm_size_resize_term()` for `RESIZE_TERM`, so sessions using the default "resize either" behavior could redraw locally without sending the remote pty resize to Mosh/vim.
* Host output decoding now has a callback/streaming API, so large unicode/full-screen server updates no longer have to fit in an 8 KiB temporary output buffer before being ACKed.
* `MOSH CONNECT` base64 must accept valid unpadded tails. Real `mosh-server` can emit a 16-byte key as 22 base64 characters without `==`; strict padded-only decoding caused PuTTY to report `Mosh SSH bootstrap ended without MOSH CONNECT`.
* Server diffs can now be up to 64 KiB and out-of-order queued diffs are allocated dynamically, avoiding large inline `cmosh_client` structs while still tolerating larger compressed server updates.
* Host protobuf decoding now skips unknown varint, length-delimited, fixed32, and fixed64 fields instead of dropping the whole server update. This avoids treating richer display/emoji-related fields as bad packets.
* PuTTY Mosh logs a one-shot Event Log diagnostic if a server update decodes successfully but contains no raw host-output bytes, which is a strong signal that the current raw-output shim needs the real terminal-state renderer for that screen.
* Upstream `hostinput.proto` confirms server diffs are still `HostMessage.instruction = 1`, extension `HostBytes = 2`, `hoststring = 4`; the Lynx emoji freeze is more likely PuTTY interpreting UTF-8 emoji bytes as 8-bit C1 terminal controls than a missing protobuf display message.
* Windows PuTTY now sets `CONF_line_codepage` to `UTF-8` by default for Mosh sessions before fonts and terminal unicode tables are initialised, but only if the user did not explicitly configure a line codepage.
* Receive-side Mosh transport now reassembles multi-fragment server instructions before zlib/protobuf decode. Previously only single final fragments decoded, so larger screen frames could be treated as bad packets, leaving the old screen visible while input continued.
* Fragment reassembly is held in `cmosh_transport_state`; `cmosh_client_recv_packet` now returns `CMOSH_CLIENT_RECV_PENDING` while waiting for remaining fragments and does not ACK a server state until the full instruction is decoded/applied.
* Fragment assembly is now cleared if a malformed/conflicting fragment arrives or if a complete single-fragment packet arrives while an old partial assembly is pending. This avoids stale fragment state poisoning later receives.
* `cmosh_client_make_input` now validates/encodes input before appending it to the retransmission queue, and rejects chunks larger than `CMOSH_CLIENT_INPUT_CHUNK_MAX`. PuTTY Mosh splits larger backend sends into protocol-sized input packets.
* The reusable session layer now also refuses input-state wrap at `UINT64_MAX`, not just the higher-level client packet constructors.
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
* If a timeout-driven PuTTY UDP reopen fails while the old socket is still present, the backend now retries socket reopen every `MOSH_UDP_REOPEN_RETRY_MS` instead of waiting for the next 30-second UDP-timeout diagnostic.
* Native PuTTY Mosh now coalesces resize requests that cannot be packetized immediately because the cmosh input retransmission queue is full or because UDP is not ready. The latest cols/rows stay pending until they can be encoded into a retransmittable input state.
* Pending PuTTY Mosh resize requests preserve ordering against terminal input already accepted by the backend. The backend records how many pending input bytes must be packetized before the resize, then sends the coalesced resize before later input bytes.
* Native PuTTY Mosh now keeps the authenticated start ACK pending if the first attempt fails locally, and retries it on timers/reopen/normal ACK paths instead of silently dropping it after the first server packet.
* Native PuTTY Mosh now caches the exact encrypted start ACK datagram and resends those same bytes for retries. Do not regenerate start ACK retries; the startup ACK uses a fixed transport nonce, so retries must be byte-identical.
* PuTTY Mosh `sendbuffer()`/`sendok()` now include pending transport-control packets (`pending_start_ack`, `pending_resize`) so PuTTY does not resume local input while ordering-critical control updates are still stuck outside the retransmission queue.
* Native PuTTY Mosh `sendok()` now requires an active UDP socket, so PuTTY applies input backpressure during local UDP outages and wakes the line discipline after a successful reopen.
* Native PuTTY Mosh now checks the datagram `sk_write()` result. A nonzero return means PuTTY's UDP layer did not send the datagram, including transient buffer-exhaustion cases; retransmitted input is made immediately retryable instead of waiting for the normal retry interval.
* `cmosh_client_note_idle_send_failed()` rolls back idle/keepalive timing after a failed UDP send, so transient local send failures do not suppress the next keepalive, timeout probe, or retransmit opportunity.
* Standalone `cmosh` now rolls back idle timing after retryable local UDP send failures for duplicate ACKs and normal server-state ACKs, matching PuTTY backend ACK retry behavior.
* Native PuTTY Mosh now logs throttled Event Log diagnostics when `sk_write()` reports a local UDP send failure.
* Native PuTTY Mosh no longer advances the association-probe retry timestamp when the cached initial UDP packet fails to send locally; the next timer tick can retry instead of waiting a full association interval.
* Failed ACK datagrams now roll back idle timing so the next timer tick can send another empty ACK/keepalive.
* PuTTY pending-input draining now stops after the first local UDP send failure. The chunk already accepted by cmosh remains in the retransmission queue, but later bytes stay in `pending_input` so backend backpressure is visible during local buffer exhaustion or interface loss.
* Core retransmit/diagnostic timers now treat backwards time as "not due yet" instead of unsigned-underflowing into immediate retransmit, missing-state, or UDP-timeout events. This covers tick wrap or clock regressions from platform timer sources.
* Receive replay history is now committed only after a decrypted packet has passed the relevant payload validation. A malformed packet with a fresh sequence no longer poisons the replay window and blocks a later valid packet using that same sequence.
* Native PuTTY Mosh backend diagnostic/probe throttles now use a local interval helper instead of raw `(uint64_t)now - last` arithmetic. This avoids log/probe storms on backwards time while preserving normal 32-bit tick wrap behavior.
* Stateful cmosh server receive now rejects packets outside the server nonce space before replay-history lookup or fragment/state mutation. Reflected client-side datagrams can no longer be consumed as server updates.
* Standalone `cmosh` bootstrap now also rejects first UDP responses outside the server nonce space, and ignores post-ACK responses outside the server nonce space. This matches the PuTTY bootstrap path and avoids treating reflected client datagrams as authenticated server startup state.
* Receive replay now rejects packets older than the retained replay window before parsing/applying them. This preserves in-window out-of-order delivery but prevents very old authenticated datagrams from re-entering processing after the ring buffer has evicted their sequence.
* Server diff queueing now rejects non-advancing state ranges and conflicting duplicate transitions for the same `old_num`/`new_num`; identical retransmitted transitions remain accepted.
* `cmosh_client_recv_packet()` now only decodes/authenticates transport packets; `cmosh_client_process_packet()` commits echo timestamps plus input ACK/throwaway trimming only after the server instruction is accepted and any applicable diff output succeeds.
* Overlapping stale server diffs (`old_num < current server_state < new_num`) are ignored at the client state-machine level instead of being queued as future gaps; authenticated ACK/throwaway fields in those packets can still trim input after processing succeeds.
* Transport decode now rejects authenticated packets whose decoded `protocol_version` does not match `CMOSH_PROTOCOL_VERSION`.
* Client receive now ignores server-state transitions whose `old_num` is beyond `CMOSH_CLIENT_SERVER_FUTURE_WINDOW` from the current state, preventing far-future packets from churning the bounded recovery queue.
* Server shutdown reporting now comes from the committed client server state, not directly from an ignored/stale packet's `new_num`. The queue selector also handles the shutdown sentinel `UINT64_MAX` as an applicable next state.
* ACK-only server transitions with no host-output diff now use the retained server-diff commit path, so server state, echo timestamp, ACK, and throwaway metadata are applied atomically with other accepted transitions.
* Duplicate server packets now retry the retained server-diff queue even when no terminal-output callback is supplied. This lets queued ACK-only/no-diff transitions drain and trim retransmission state during duplicate recovery, while real output diffs still fail if there is no output sink.

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
* If local UDP sending is failing, do not continue draining the PuTTY-side pending-input buffer into more cmosh states in the same loop; stop after the failed datagram and let normal retry/backpressure resume.
* A PuTTY resize event accepted by the backend must either remain in `pending_resize` or be encoded into cmosh retransmission state. Do not silently drop resize updates just because the input retransmission queue is temporarily full.
* Pending PuTTY resize updates are ordered after input bytes already accepted into the backend and before later pending input bytes. Multiple resize events coalesce to the latest cols/rows at that ordering point.
* The start ACK generated from the first authenticated server packet is ordering-critical startup state. If local UDP send fails, keep it pending and retry the original encrypted datagram; `sendok()` must remain false while it is pending.
* Do not use unsigned elapsed-time subtraction unless the current timestamp is known to be at least the stored timestamp; backwards time is not evidence that retransmit or timeout is due.
* Do not add a received sequence to replay history until the decrypted packet is structurally valid enough to be consumed. Partial fragments are recorded after a valid fragment is accepted into assembly.
* Stateful client receive only accepts server nonce-space packets. Reflected client packets must not affect replay, fragment reassembly, ACK, or terminal state.
* Receive replay only tolerates out-of-order packets inside the retained replay window. Packets older than that window are treated as replay even if their exact sequence has fallen out of the ring buffer.
* A queued server diff must advance state, and the same state transition must have stable bytes if it is seen more than once.
* Input ACK/throwaway trimming is committed by the high-level receive path only after server-state validation. Bad authenticated state transitions must not discard queued input.
* Stale overlapping display diffs cannot be applied from the current server state and must not consume future-diff queue slots.
* A duplicate server packet may still be useful if a prior output callback failure left an applicable server diff queued. The high-level receive path now retries queued applicable diffs on duplicate packets, while preserving duplicate classification for callers. PuTTY and standalone `cmosh` duplicate handling must still honor server shutdown after such a retry.
* While reassembling fragmented transport instructions, an older fragment ID must not clear a newer in-progress fragmented instruction. Newer IDs may still supersede older incomplete assemblies.
* A complete single-fragment older packet may decode independently while a newer fragmented instruction is pending, but must not clear the newer partial assembly.
* Queued server diffs now retain authenticated ACK/throwaway metadata and the packet timestamp. If host output fails once and a later duplicate drains the retained diff, input trimming and echo timestamp update complete with that retained metadata instead of leaving retransmission state stale.
* When the bounded server-diff queue is full, preserve the closest recoverable state transitions. A nearer incoming diff may evict the farthest future diff, while a too-far future diff is dropped instead of displacing closer recovery state.
* Far-future server transitions outside `CMOSH_CLIENT_SERVER_FUTURE_WINDOW` must not enter the future-diff queue. They may still carry authenticated input ACK/throwaway information after high-level validation.
* Remote shutdown is a committed server-state transition. Stale or ignored packets with `new_num == UINT64_MAX` must not notify remote exit unless the state machine actually advances to `CMOSH_CLIENT_SERVER_SHUTDOWN_STATE`.
* Decoded transport instructions must match `CMOSH_PROTOCOL_VERSION` before client state, ACK trimming, or host output can be affected.
* No-diff server-state transitions still advance state and commit retained ACK/throwaway metadata. Stale no-diff transitions must not advance server state, though their authenticated ACK/throwaway fields may still trim input after high-level validation.
* Duplicate packet recovery must be able to apply queued no-diff transitions without requiring a host-output callback; requiring output would leave ACK-only metadata stuck behind duplicate replay.
* Client input retransmission records now store the encoded user diff, not the raw key bytes. Queue capacity checks must use encoded diff length so packetized input and retained retransmission state agree.
* PuTTY and standalone `cmosh` may shrink an input chunk when encoded-diff queue space is tight; unsent local bytes must remain pending instead of being dropped or turning queue pressure into a fatal disconnect.
* Standalone `cmosh` resize attempts that cannot be packetized because the retransmission queue is full are deferred by leaving `last_cols`/`last_rows` unchanged; the resize will be retried after input ACKs free queue space.
* Client init now only seeds receive replay history from a server nonce-space initial packet. Startup paths should reject authenticated first UDP packets that do not advance server state.
* PuTTY Mosh caps pre-`MOSH CONNECT` bootstrap output at 64 KiB with overflow-safe accounting; the bootstrap parser only needs the startup lines, so unbounded stderr/stdout is treated as fatal.
* Base64 bootstrap key decoding accepts valid unpadded tails because `mosh-server` can emit unpadded session keys. It still rejects impossible one-character tails, excessive padding, and data after padding.
* Protobuf varint decoding rejects uint64 overflow, transport instructions reject field 0, and unknown fixed32/fixed64 transport fields are skipped instead of dropping otherwise valid packets.
* Unknown protobuf fields with valid fixed32/fixed64/length-delimited wire data can be skipped, but truncated fixed-width or length-delimited unknown fields remain malformed.
* Fragment/zlib encoder capacity checks must be subtraction-based (`payload_len > outlen - header`) so invalid caller lengths cannot wrap addition-based checks.
* OCB and transport packet encryption capacity checks must be subtraction-based before pointer arithmetic or encryption loops run. Oversized caller lengths must fail immediately.
* Host-output protobuf decoding also rejects field 0, while still skipping unknown valid wire types. Output copy and protobuf buffer helpers use subtraction-based bounds checks to avoid size_t wraparound.
* Input/resize packet constructors now refuse to enqueue when the local input state is already `UINT64_MAX`, preventing client-state wraparound in encoded packets.
* Standalone `cmosh` ignores optional post-start-ACK packets that do not advance server state and falls back to the first authenticated server state.
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
* Latest `cmake --build build --target otherbackends --config Debug` passed after PuTTY UDP-send diagnostics, ACK retry rollback, and pending-input drain limiting.
* Latest `cmake --build build --target putty --config Debug` passed after PuTTY UDP-send diagnostics, ACK retry rollback, and pending-input drain limiting.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after backwards-time guards and replay-history validation hardening.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after backwards-time guards and replay-history validation hardening.
* Latest `cmake --build build --target putty --config Debug` passed after backwards-time guards and replay-history validation hardening.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after backend timer-throttle and server nonce-space hardening.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after backend timer-throttle and server nonce-space hardening.
* Latest `cmake --build build --target putty --config Debug` passed after backend timer-throttle and server nonce-space hardening.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after replay-window and server-diff queue hardening.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after replay-window and server-diff queue hardening.
* Latest `cmake --build build --target putty --config Debug` passed after replay-window and server-diff queue hardening.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after validated ACK commit and stale-overlap hardening.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after validated ACK commit and stale-overlap hardening.
* Latest `cmake --build build --target putty --config Debug` passed after validated ACK commit and stale-overlap hardening.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after duplicate-drain and stale-fragment hardening.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after duplicate-drain and stale-fragment hardening.
* Latest `cmake --build build --target putty --config Debug` passed after duplicate-drain and stale-fragment hardening.
* Latest `cmake --build build --target cmosh --config Debug` passed after standalone duplicate-drain shutdown handling.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after standalone duplicate-drain shutdown handling.
* Latest `cmake --build build --target putty --config Debug` passed after standalone duplicate-drain shutdown handling.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after server-diff queue overflow and retained metadata hardening.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after server-diff queue overflow and retained metadata hardening.
* Latest `cmake --build build --target cmosh --config Debug` passed after server-diff queue overflow and retained metadata hardening.
* Latest `cmake --build build --target putty --config Debug` passed after server-diff queue overflow and retained metadata hardening.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after stale complete-fragment preservation.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after stale complete-fragment preservation.
* Latest `cmake --build build --target otherbackends --config Debug` passed after timeout-driven UDP reopen retry hardening.
* Latest `cmake --build build --target cmosh --config Debug` passed after standalone ACK send-failure handling.
* Latest `cmake --build build --target putty --config Debug` passed after timeout-driven UDP reopen retry hardening.
* Latest `cmake --build build --target otherbackends --config Debug` passed after pending resize coalescing/retry hardening.
* Latest `cmake --build build --target putty --config Debug` passed after pending resize coalescing/retry hardening.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after pending resize coalescing/retry hardening.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after pending resize coalescing/retry hardening.
* Latest `cmake --build build --target otherbackends --config Debug` passed after pending start-ACK retry and control-packet backpressure hardening.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after pending start-ACK retry and control-packet backpressure hardening.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after pending start-ACK retry and control-packet backpressure hardening.
* Latest `cmake --build build --target putty --config Debug` passed after pending start-ACK retry and control-packet backpressure hardening.
* Latest `cmake --build build --target otherbackends --config Debug` passed after byte-identical start ACK retry hardening.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after byte-identical start ACK retry hardening.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after byte-identical start ACK retry hardening.
* Latest `cmake --build build --target putty --config Debug` passed after byte-identical start ACK retry hardening.
* Latest `cmake --build build --target cmosh --config Debug` passed after byte-identical start ACK retry hardening.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after standalone bootstrap server nonce-space hardening.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after standalone bootstrap server nonce-space hardening.
* Latest `cmake --build build --target cmosh --config Debug` passed after standalone bootstrap server nonce-space hardening.
* Latest `cmake --build build --target putty --config Debug` passed after standalone bootstrap server nonce-space hardening.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after protocol-version, far-future-state, and shutdown-sentinel hardening.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after protocol-version, far-future-state, and shutdown-sentinel hardening.
* Latest `cmake --build build --target cmosh --config Debug` passed after protocol-version, far-future-state, and shutdown-sentinel hardening.
* Latest `cmake --build build --target putty --config Debug` passed after protocol-version, far-future-state, and shutdown-sentinel hardening.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after no-diff server-transition metadata hardening.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after no-diff server-transition metadata hardening.
* Latest `cmake --build build --target putty --config Debug` passed after no-diff server-transition metadata hardening.
* Latest `cmake --build build --target cmosh --config Debug` passed after no-diff server-transition metadata hardening.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after duplicate no-diff queue-drain hardening.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after duplicate no-diff queue-drain hardening.
* Latest `cmake --build build --target putty --config Debug` passed after duplicate no-diff queue-drain hardening.
* Latest `cmake --build build --target cmosh --config Debug` passed after duplicate no-diff queue-drain hardening.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after encoded-input retransmission and pending-input chunk shrinking.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after encoded-input retransmission and pending-input chunk shrinking.
* Latest `cmake --build build --target cmosh --config Debug` passed after standalone pending-input/resize deferral hardening.
* Latest `cmake --build build --target putty --config Debug` passed after PuTTY pending-input chunk shrinking.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after startup nonce/state and bootstrap-output hardening.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after startup nonce/state and bootstrap-output hardening.
* Latest `cmake --build build --target cmosh --config Debug` passed after standalone first-server-state validation.
* Latest `cmake --build build --target putty --config Debug` passed after first-server-state validation and bootstrap-output cap.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after base64/protobuf parser hardening and standalone post-ACK state validation.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after base64/protobuf parser hardening and standalone post-ACK state validation.
* Latest `cmake --build build --target cmosh --config Debug` passed after base64/protobuf parser hardening and standalone post-ACK state validation.
* Latest `cmake --build build --target putty --config Debug` passed after base64/protobuf parser hardening and standalone post-ACK state validation.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after host-output field-zero rejection, overflow-safe bounds checks, duplicate-connect rejection, and input-state wrap prevention.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after host-output field-zero rejection, overflow-safe bounds checks, duplicate-connect rejection, and input-state wrap prevention.
* Latest `cmake --build build --target cmosh --config Debug` passed after host-output field-zero rejection, overflow-safe bounds checks, duplicate-connect rejection, and input-state wrap prevention.
* Latest `cmake --build build --target putty --config Debug` passed after host-output field-zero rejection, overflow-safe bounds checks, duplicate-connect rejection, and input-state wrap prevention.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after fixing unpadded `MOSH CONNECT` base64 acceptance and adding malformed unknown-field/bounds regression tests.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after fixing unpadded `MOSH CONNECT` base64 acceptance and adding malformed unknown-field/bounds regression tests.
* Latest `cmake --build build --target cmosh --config Debug` passed after fixing unpadded `MOSH CONNECT` base64 acceptance and adding malformed unknown-field/bounds regression tests.
* Latest `cmake --build build --target putty --config Debug` passed after fixing unpadded `MOSH CONNECT` base64 acceptance and adding malformed unknown-field/bounds regression tests.
* Latest `git diff --check` passed with only expected CRLF conversion warnings.
* Latest `cmake --build build --target test_cmosh --config Debug` passed after OCB/transport oversized-length checks and session input-state wrap hardening.
* Latest `.\build\cmosh\Debug\test_cmosh.exe` passed after OCB/transport oversized-length checks and session input-state wrap hardening.
* Latest `cmake --build build --target cmosh --config Debug` passed after OCB/transport oversized-length checks and session input-state wrap hardening.
* Latest `cmake --build build --target putty --config Debug` passed after OCB/transport oversized-length checks and session input-state wrap hardening.
* Latest `git diff --check` passed with only expected CRLF conversion warnings.

## Known Issues

* Full terminal correctness is not complete; output still depends on raw host-output decoding instead of a full Mosh terminal-state model.
* High-latency or lossy links may still show repeated characters; throwaway handling and resize retransmission are only mitigations.
* Sleep/wake and interface changes should now survive transient local UDP socket close/reopen failures better, but still need live Windows testing with the freshly rebuilt `build\Debug\putty.exe`.
* Up-arrow-after-login issue still needs investigation if it persists after the UTF-8/default rebuild; likely candidates are startup tty modes or local line discipline state before UDP readiness.

## Exact Next Step

Retest the freshly rebuilt `build\Debug\putty.exe` on startup and lossy-link paths: sleep/wake, local network loss/recovery, paste bursts, rapid command-history navigation immediately after login, resize/maximize/restore under load, and large/fragmented screen updates. Watch for throttled UDP send and input ACK/throwaway Event Log lines to correlate any repeated-character reports. Protocol hardening is now mostly down to live validation and any bugs surfaced by that testing; remaining major implementation gap is still the full Mosh terminal-state renderer.
