# cmosh Remaining Work Plan

## Protocol Hardening Status

The transport/protocol layer is mostly hardened for the known failure modes:

* authenticated packets are constrained to server nonce space before replay/state mutation
* replay history rejects duplicates and packets outside the retained window
* fragmented server instructions are reassembled, with stale/conflicting fragment cleanup
* server state transitions are validated for protocol version, stale overlap, far-future gaps, duplicate conflicts, shutdown sentinel, and no-diff metadata
* input ACK and throwaway trimming commits only after server state validation
* input/resize records are retransmittable encoded diffs, with state-wrap prevention
* UDP send failures roll back retry timing and preserve ownership of queued input/control state
* PuTTY backend applies backpressure during UDP outage, pending start ACK, pending resize, and queued input
* parser/capacity checks cover base64, protobuf varints/fields, fragment/zlib, OCB, and packet encryption bounds

Remaining protocol work should be driven by live failures, not broad rewrites.

## Protocol Validation Tasks

1. Run live lossy-link tests against `build\Debug\putty.exe -load rhea-mosh`.
2. Capture Event Log lines for:
   * `Mosh UDP send failed locally`
   * input retransmit diagnostics
   * server input ACK/throwaway diagnostics
   * missing-state and UDP-timeout diagnostics
3. For repeated-character reports, compare retransmit state numbers against ACK/throwaway trims.
4. For sleep/wake or network-change reports, confirm UDP reopen happens before remote disconnect.
5. For startup failures, preserve the SSH bootstrap output and first UDP packet diagnostic.
6. Add targeted unit tests only for any newly observed state-machine failure.

## Terminal-State Renderer Plan

### 1. Define Renderer Boundary

Add a reusable `cmosh_terminal` layer, separate from PuTTY and standalone glue.

Inputs:
* decoded Mosh host instructions / display updates
* resize events
* reset/redraw requests

Outputs:
* terminal bytes or callbacks representing a complete redraw/delta suitable for PuTTY `seat_stdout`
* dirty-region diagnostics for tests

Keep PuTTY-specific terminal APIs out of this layer initially.

### 2. Decode Host State, Not Just Raw Bytes

Extend host protobuf decoding beyond raw `HostBytes` extraction:

* HostMessage instruction list
* Complete / Display messages
* EchoAck
* terminal dimensions
* cursor position and visibility
* attributes
* alternate screen state
* scroll regions

Unknown valid fields should continue to be skipped; malformed fields should fail.

### 3. Implement Terminal Screen Model

Maintain explicit state:

* primary and alternate screen buffers
* cells with Unicode codepoint/grapheme representation, width, attributes, and dirty flag
* cursor row/column
* current attributes
* scroll region
* wrap mode and pending wrap
* resize behavior

Start conservative: correctness first, no speculative local echo.

### 4. Render Deterministic Deltas

Render from state changes rather than appending decoded host strings.

Initial renderer can emit a full redraw after each committed display update:
* reset attributes
* home cursor
* write every visible row
* clear to end of line where needed
* restore cursor

After correctness is stable, optimize to dirty-region deltas.

### 5. Wire Into cmosh Client

Replace `mosh_host_output` raw-byte decode with:

1. decode host update into `cmosh_terminal`
2. apply update only after transport state validation
3. render resulting delta/full redraw
4. send rendered bytes to `seat_stdout`

On output failure, keep the retained server diff behavior already implemented.

### 6. Reset Terminal / Redraw

Reset Terminal should request a redraw from the terminal model.

If a full renderer exists:
* emit full current screen from model
* preserve Mosh protocol state
* do not force fake remote resize unless needed as fallback

Until then, the existing resize/redraw hook remains a temporary workaround.

### 7. Tests

Add unit tests before broad UI testing:

* colored `ls` attributes
* prompt redraw and clear-to-end-line
* cursor movement and overwrite
* scroll and scroll region
* alternate screen enter/exit
* vim open/insert/exit
* resize larger/smaller
* Unicode wide characters and emoji
* EchoAck without visible output
* retained output failure followed by duplicate packet recovery

Then run live PuTTY tests:
* bash prompt
* colored `ls`
* vim
* lynx with emoji/user-generated content
* maximize/restore
* sleep/wake and network loss

## Small Adjacent Tasks

* Keep `CMOSH_HANDOFF.md` current after each checkpoint.
* Add Event Log wording for renderer fallback/state-only updates once the renderer boundary exists.
* Review bootstrap locale/server options for configurability after renderer work starts.
* Consider a bounded PuTTY-side `pending_input` policy once behavior under large paste and UDP outage is measured.
