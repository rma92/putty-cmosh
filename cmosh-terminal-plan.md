# CMOSH Screen Buffer Implementation Plan

  ## Summary

  Implement the first screen-buffer milestone as a small C-native cmosh_terminal layer in cmosh, not a
  C++ port of upstream Mosh and not a PuTTY terminal-internals hook.

  The layer will parse the terminal byte stream carried in Mosh HostBytes.hoststring, maintain an
  explicit framebuffer, and emit deterministic full-screen redraw bytes for PuTTY/standalone output.
  Keep dirty-region rendering and speculative local echo out of this milestone.

  ## Key Changes

  - Add cmosh/cmosh_terminal.h and cmosh/cmosh_terminal.c, included in both cmosh_core and
    cmosh_core_embedded.

  - Model:
      - visible screen buffer with fixed cols/rows
      - cell text as UTF-8 bytes, width, attributes, dirty flag
      - cursor row/col, cursor visibility
      - current SGR attributes including bold/italic/underline/blink/inverse/invisible, ANSI/256/
        truecolor fg/bg

      - scroll region, wrap mode, pending wrap
      - title/clipboard/hyperlink parsing can be accepted/skipped initially without corrupting cell
        state

  - Parser scope for v1:
      - printable UTF-8, CR, LF, BS, BEL
      - CSI H/f, A/B/C/D, K, J, X, m, r
      - DEC private modes used by Mosh display output: cursor visibility ?25, reverse video ?5,
        bracketed paste/mouse modes accepted as state/no-op

      - OSC 0, 1, 2, 52, and 8 parsed enough to consume BEL/ST terminators safely

  - Host protobuf handling:
      - keep existing malformed-field rejection and unknown-valid-field skipping
      - add a richer host-message apply path that handles hostbytes, resize, and echoack
      - hostbytes applies bytes to cmosh_terminal
      - resize resizes the terminal model before applying later instructions in the same host message
      - echoack is accepted as non-visible metadata

  - Rendering:
      - initial render emits full redraw after every accepted visible update: reset attributes, clear
        screen, home cursor, write all rows, clear trailing row content as needed, restore cursor and
        visibility

      - output via callback/strbuf so existing output-failure behavior remains atomic with
        cmosh_client_apply_server_diffs

      - no dirty-region optimization in this milestone

  - Integration:
        resize instructions

      - replace mosh_host_output raw byte forwarding with terminal apply + full redraw to seat_stdout
      - keep the existing “no raw host-output bytes” log only as a temporary diagnostic if the new apply
        path sees no visible change

      - update standalone cmosh to use the same terminal layer for parity

  ## Public Interfaces

  - cmosh_terminal_new(cols, rows) / cmosh_terminal_free
  - cmosh_terminal_resize(term, cols, rows)
  - cmosh_terminal_apply_bytes(term, data, len)
  - cmosh_terminal_render_full(term, output_cb, ctx)
  - cmosh_decode_host_apply(...) or equivalent host-message walker that calls terminal apply/resize/echo
    handlers in instruction order

  All APIs return failure on malformed input, allocation failure, or output callback failure.

  ## Test Plan

  - Add focused test_cmosh coverage for:
      - simple prompt text and cursor placement
      - overwrite and clear-to-end-line
      - SGR colors/attributes, including reset
      - cursor movement and relative movement
      - scroll region and LF scrolling
      - resize larger/smaller
      - UTF-8 wide characters and combining/emoji smoke cases using mk_wcwidth
      - EchoAck-only host message produces no visible redraw requirement
      - host ResizeMessage followed by HostBytes applies in order
      - malformed CSI/OSC/protobuf input fails without committing partial state where practical

  - Verification commands:
      - cmake --build build --target test_cmosh --config Debug
      - .\build\cmosh\Debug\test_cmosh.exe
      - cmake --build build --target cmosh --config Debug
      - cmake --build build --target putty --config Debug
      - git diff --check

  ## Assumptions

  - First milestone targets correctness over bandwidth; full redraw after each accepted update is
    acceptable.

  - The parser is scoped to Mosh Display::new_frame output, not arbitrary remote application terminal
    output.

  - PuTTY remains the final terminal renderer; cmosh_terminal only reconstructs Mosh-visible state and
    emits conservative VT redraw bytes.

  - No public PuTTY terminal internals are changed.
  - Dirty-region deltas, local echo prediction, and complete upstream Mosh terminal parity are deferred
    until the full-redraw model is stable.
