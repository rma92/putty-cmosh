# cmosh / PuTTY Mosh Conversation Context

This file is a compact continuity note for starting a fresh conversation.
Read it together with `CMOSH_HANDOFF.md` and `CMOSH_REMAINING_PLAN.md`.

## Original Direction

The work is implementing native Mosh support in PuTTY while stabilizing the
standalone `cmosh` client.

Key intent:

* keep native PuTTY SSH bootstrap, not external `plink`
* preserve PuTTY auth, Pageant, host-key prompts, proxy settings, saved sessions
* build reusable cmosh protocol/client layers that PuTTY can call
* prioritize correctness over speculative local echo
* eventually replace raw host-output rendering with a real Mosh terminal-state
  renderer

## User Feedback Timeline

Early terminal rendering:

* Initial buffered/raw rendering produced badly scrambled full-screen output.
* Later testing showed vim was not regressed, though `ls` still did not clear the
  screen as expected.
* PuTTY auth and native SSH bootstrap worked, including key authentication.
* PuTTY reached real Mosh connectivity.

Transport/network behavior:

* User saw `mosh: UDP timeout; no server packet received recently` on poor links.
* User saw repeated characters on high-latency/mobile connections.
* User tested sleep/wake and once saw a `No Buffer space` error.
* Work then focused heavily on UDP send failure recovery, retry timing,
  retransmission ownership, socket reopen, and backpressure.

Window/resize behavior:

* Resize generally worked.
* Maximize/restore did not resize vim until Windows maximize/restore was wired
  through the same terminal resize path as ordinary `WM_SIZE`.
* Reset Terminal should eventually trigger a full redraw from the terminal model;
  current resize/redraw behavior is only a workaround.

Lynx/Unicode behavior:

* Lynx pages with emoji/user-generated content initially caused freezes or blank
  stale screens.
* Receive-side multi-fragment reassembly and host protobuf unknown-field
  tolerance fixed the observed Lynx issue.
* Windows PuTTY now defaults Mosh sessions to UTF-8 line codepage unless the user
  explicitly configured a different codepage.

Startup regression:

* A protocol-hardening pass made base64 parsing too strict.
* User reported PuTTY fatal: `Mosh SSH bootstrap ended without MOSH CONNECT`.
* Cause: real `mosh-server` can emit the 16-byte key as 22 unpadded base64
  characters.
* Fix: accept valid unpadded base64 tails while still rejecting impossible
  one-character tails, excessive padding, and data after padding.

## Major Implemented Areas

PuTTY native Mosh:

* Added native Mosh backend path using PuTTY SSH bootstrap.
* Bootstrap output is captured by a wrapper seat and parsed for `MOSH CONNECT`
  and `MOSH IP`.
* UDP startup uses cached byte-identical association probes and start ACK
  retries.
* PuTTY terminal input, resize, `sendbuffer()`, `sendok()`, and UDP retries are
  connected to the cmosh client state machine.
* PuTTY logs throttled diagnostics for UDP send failures, retransmits, and
  input ACK/throwaway trims.

Protocol/transport hardening:

* Receive packets must be in server nonce space before replay/state mutation.
* Replay history rejects duplicates and packets outside the retained window.
* Fragmented server instructions are reassembled before zlib/protobuf decode.
* Malformed/conflicting fragments clear assembly; older stale fragments do not
  poison newer assemblies.
* Server transitions validate protocol version, stale overlap, far-future gaps,
  duplicate conflicts, shutdown sentinel, and no-diff metadata.
* ACK and throwaway trimming only commits after validated server processing.
* Duplicate packets can drain retained queued diffs, including no-diff metadata.
* Input/resize records store encoded diffs and retransmit exactly those bytes.
* Send failures roll back retry timing and preserve input/control ownership.
* UDP socket close/error is treated as local recoverable outage where possible.
* Backwards time no longer forces immediate retransmit/timeout storms.
* Capacity checks were hardened in base64/protobuf/zlib/fragment/OCB/transport
  paths.
* Direct session input append now refuses client state wrap at `UINT64_MAX`.

Tests:

* `test_cmosh` has broad coverage for base64, AES/OCB, protobuf, zlib fragment,
  packet crypto, replay, malformed packet replay poisoning, nonce-space
  rejection, fragment reassembly, server diff queues, input retransmission,
  ACK/throwaway behavior, no-diff metadata, duplicate recovery, shutdown
  handling, oversized length rejection, and state wrap prevention.

## Current Known Issues

* Full terminal correctness is not complete.
* Output still depends on raw host-output byte extraction, not a full Mosh
  terminal-state model.
* High-latency/lossy repeated-character behavior needs continued live testing.
  Existing diagnostics should be used to correlate repeats with retransmit and
  ACK/throwaway state.
* Sleep/wake and network interface changes need more Windows live testing.
* The early up-arrow-after-login issue may still need investigation if it
  persists.

## Current Best Next Step

Do not restart the architecture. Continue from:

1. `CMOSH_HANDOFF.md`
2. `CMOSH_REMAINING_PLAN.md`
3. current `git status`
4. targeted diffs/tests

Recommended next implementation focus:

* Begin the reusable `cmosh_terminal` renderer layer described in
  `CMOSH_REMAINING_PLAN.md`.
* Start with a correct full-screen redraw from maintained terminal state rather
  than dirty-region optimization.
* Keep host protobuf decoding tolerant of unknown valid fields and strict about
  malformed fields.
* Preserve the existing validated transport/ACK/retransmission path; terminal
  output application must remain after server-state validation.

Recommended live validation:

* `build\Debug\putty.exe -load rhea-mosh`
* Test startup, sleep/wake, lossy/mobile link, paste bursts, vim, lynx with
  emoji-heavy pages, maximize/restore, and Reset Terminal.
* Use Event Log retransmit and ACK/throwaway lines to debug repeated characters.

## Verification Pattern

For protocol/core changes:

```powershell
cmake --build build --target test_cmosh --config Debug
.\build\cmosh\Debug\test_cmosh.exe
cmake --build build --target cmosh --config Debug
cmake --build build --target putty --config Debug
git diff --check
```

For PuTTY-backend-only changes:

```powershell
cmake --build build --target otherbackends --config Debug
cmake --build build --target putty --config Debug
git diff --check
```

`git diff --check` commonly reports only CRLF conversion warnings in this tree;
those are expected unless actual whitespace errors appear.
