# cmosh

`cmosh` is a clean-room C client skeleton for bootstrapping an existing
`mosh-server` over an external SSH client.

Current implemented pieces:

- CMake targets: `cmosh`, `cmosh_core`, and `test_cmosh`.
- CLI parsing for the planned standalone interface.
- External SSH bootstrap using `plink` on Windows and `ssh` elsewhere, with
  `--ssh=` override.
- Remote `mosh-server new` command construction.
- UTF-8 remote locale prefixing with `--locale=` override.
- Parser for `MOSH IP` and `MOSH CONNECT <port> <base64-key>` startup lines.
- UDP address resolution and datagram socket setup for the server target.
- AES-128 block encryption, OCB3 authenticated encryption, nonce-prefixed
  datagram encryption/decryption, minimal protobuf wire helpers, and initial
  transport sequence replay checks.
- Client/server crypto direction constants for mosh wire sequence numbers.
- Transport-instruction protobuf encode/decode for protocol version, state
  numbers, acknowledgements, throwaway numbers, diff payloads, and chaff.
- Minimal user-message resize protobuf encoder.
- Minimal zlib stored-block compressor/decompressor and single-fragment packet
  wrapper, including the final-fragment flag.
- Encrypted UDP association probe carrying a protocol-v2 state update with a
  resize diff, zlib-compressed and wrapped as one final fragment, plus a short
  retransmitting authenticated receive poll.
- Decode authenticated server fragments into transport instructions when they
  use the supported stored-block zlib form.
- Send an authenticated acknowledgement for the first decoded server state.
- Decode basic HostMessage hoststring diffs and write the resulting bytes to
  stdout.
- Bounded bootstrap has been replaced by a simple session loop that receives
  host output, sends keystrokes, sends periodic ACK keepalives, and exits on
  Ctrl+]. The initial resize uses the current console size; live resize updates
  are deferred until the client state model is more complete.
- Normal mode is intended to show only terminal I/O. `--verbose` enables the
  bootstrap and protocol trace.
- Windows console output enables virtual-terminal processing before entering
  the UDP session.

The full mosh state-sync/UDP session loop is not implemented yet. The
executable intentionally exits after successful bootstrap and UDP setup with a
clear diagnostic instead of pretending to speak an incomplete protocol.
