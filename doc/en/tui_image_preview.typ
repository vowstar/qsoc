= TUI LIVE IMAGE PREVIEW
<tui-image-preview>
The agent TUI displays bitmap attachments (PNG, JPG, GIF, WebP) inline
in the chat history on terminals that speak the kitty graphics
protocol or the iTerm2 inline image protocol. The image lands in a
reserved cell rectangle right under its `[image: ...]` label, scrolls
with the rest of the conversation, and disappears cleanly when the
agent exits. Terminals without a graphics protocol still see the
metadata line so the conversation stays readable.

== TERMINAL COMPATIBILITY MATRIX
<tui-image-preview-matrix>

#figure(
  align(center)[#table(
    columns: (1fr, 0.7fr, 1.5fr),
    align: (left, center, left),
    table.header([Terminal], [Live preview], [Detection signal]),
    table.hline(),
    [kitty], [yes (kitty)], [`KITTY_WINDOW_ID`, `TERM=xterm-kitty`],
    [ghostty], [yes (kitty)], [`GHOSTTY_RESOURCES_DIR`, `TERM_PROGRAM=ghostty`, `TERM=xterm-ghostty`],
    [WezTerm], [yes (kitty)], [`WEZTERM_EXECUTABLE`, `TERM_PROGRAM=WezTerm`],
    [Konsole], [yes (kitty)], [`KONSOLE_VERSION`],
    [Rio], [yes (kitty)], [`TERM_PROGRAM=rio`],
    [iTerm2], [yes (OSC 1337)], [`TERM_PROGRAM=iTerm.app`],
    [VS Code], [yes (OSC 1337)], [`TERM_PROGRAM=vscode`],
    [mintty], [yes (OSC 1337)], [`TERM=mintty`, `TERM_PROGRAM=mintty`],
    [foot, mlterm, contour], [placeholder text only], [`TERM` substring],
    [other], [placeholder text only], [no signal],
    [tmux / screen], [placeholder text only], [`TMUX` or `STY` set],
  )]
)

The detection runs on every layout call so a test or shell can flip
the environment between sessions without restarting the agent.

== CELL-RECTANGLE RESERVATION
<tui-image-preview-rect>
A graphics-aware terminal lets a program place a bitmap into a cell
rectangle of `(N cols, M rows)`. QSoC computes that rectangle from
the image's pixel dimensions:

- `cellCols = clamp(image_w / 8, 8, viewportWidth - 2)`
- `cellRows = round(cellCols * image_h / image_w * (8 / 16))`
- `cellRows` is then clamped to `[4, 30]` so neither a wide banner
  nor a tall portrait dominates the chat

The rectangle reservation is a property of the layout: the block
reports `rowCount() = 1 + cellRows` so the scroll view leaves the
correct vertical room above the next block. Inside that rectangle
the cell-grid pass paints empty cells; the actual bitmap lands later
through the graphics overlay.

== KITTY: TRANSMIT ONCE, PLACE PER FRAME
<tui-image-preview-kitty>
The kitty graphics protocol separates "upload bitmap" from "place
bitmap". QSoC uploads the PNG bytes a single time using
`a=t,i=N,f=100` chunked transfer:

```
ESC _ G i=<id>,a=t,f=100,q=2,m=1; <chunk1> ESC \
ESC _ G q=2,m=1; <chunk2> ESC \
...
ESC _ G q=2,m=0; <last> ESC \
```

Subsequent compositor frames only re-emit the lightweight placement
escape after a cursor jump:

```
ESC [ <row> ; <col> H
ESC _ G a=p,i=<id>,p=1,c=<W>,r=<H>,C=1,q=2 ESC \
```

The `p=1` placement id keeps the same rectangle reused across frames
so back-to-back emits at the same coords are no-ops. `C=1` keeps the
cursor where it was before the placement so the cell-grid diff stays
stable. `q=2` suppresses the per-command OK reply that would
otherwise race the agent stdin reader. JPEG, GIF and WebP
attachments are decoded through QImage and re-encoded as PNG before
transmit because the kitty `f=100` slot only accepts PNG.

When a block scrolls out of the viewport the scroll view emits
`a=d,d=p,i=<id>,p=1,q=2`, which deletes only that one placement and
keeps the bitmap cached for a future scroll-in. On compositor stop
every transmitted image is freed via `a=d,d=I,i=<id>,q=2`. The
kitty spec has no `a=D` action; uppercase only ever appears as the
`d=` selector to choose between "keep image data" (lowercase) and
"free image data" (uppercase).

== ITERM2: PER-FRAME OSC 1337 WITH THROTTLE
<tui-image-preview-iterm2>
iTerm2 has no transmit-once primitive: every emission embeds the
full base64 payload. To keep the 100 ms compositor tick from
re-uploading megabytes for an image that has not moved, the block
remembers the last placement coords and skips emission when neither
the row nor the column changed since the previous frame:

```
ESC [ <row> ; <col> H
ESC ] 1337 ; File=name=<b64>;size=<N>;
            inline=1;preserveAspectRatio=1;width=<W>;height=<H>:<b64> BEL
```

Both `width` and `height` are in cell units, matching the rectangle
the cell-grid pass already reserved. iTerm2 keeps the bitmap in
place until the underlying cells are overwritten, so a stable frame
costs nothing.

== LIFECYCLE
<tui-image-preview-lifecycle>
Per visible block, the scroll view drives three signals:

- *enter viewport* -- block returns its placement escape from
  `emitGraphicsLayer`; the kitty path also runs the upload on the
  first call, the iTerm2 path emits the inline image once
- *leave viewport* (scroll out) -- block returns `a=d,d=i,i=N,p=1`
  from `emitGraphicsClear` (kitty) or resets its iTerm2 throttle so
  the next re-entry uploads again
- *compositor stop* -- block returns `a=D,d=i,i=N` from
  `emitGraphicsDestroy` so the terminal frees the bitmap; runs
  before the alt screen unwinds

A scroll out followed by a scroll back in costs only the placement
escape on kitty (the upload cache survives) and the full inline
image on iTerm2 (no protocol-level cache to reuse).

== MULTIPLEXER AND OPT-OUT GATING
<tui-image-preview-gating>
Two gates force the text-only fallback before any cell rectangle
gets reserved, so a session that cannot actually display the image
does not waste vertical space:

- `TMUX` or `STY` env var present: tmux and GNU screen both strip
  the kitty / iTerm2 graphics escapes by default, so the host
  terminal would never see the bitmap. The detector returns
  text-only in this case. tmux has an opt-in passthrough mode but
  it requires a host config change plus DCS wrapping; that is out
  of scope for the gate.
- `QSOC_NO_IMAGE_GRAPHICS` set to a non-empty value: explicit user
  opt-out for bandwidth-conscious SSH sessions or any context
  where the user prefers a single placeholder line over a
  best-effort image.

Both gates take effect at the layout stage, so the cell-grid
footprint of the block stays at one row and the chat history
keeps reading naturally.

== CONFIGURATION HOOKS
<tui-image-preview-config>
Live preview activates automatically whenever the detection picks
up a graphics-capable terminal and neither gate above fires. There
is no `[tui]` knob to turn it off; on a text-only terminal the
placeholder line is the same one non-graphical builds always
rendered, so the change is invisible.
