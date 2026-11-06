= TUI Live Image Preview
<tui-image-preview>
The agent TUI shows bitmap attachments (PNG, JPG, GIF, WebP) inline.
Terminals with the kitty graphics, iTerm2 inline image, or Sixel protocol show
the bitmap. Other color terminals show a half-block preview in terminal cells.
Fine text in a half-block preview is not readable.

== Terminal Compatibility Matrix
<tui-image-preview-matrix>

#figure(
  align(center)[#table(
    columns: (1fr, 0.7fr, 1.5fr),
    align: (left, center, left),
    table.header([Terminal], [Preview], [Detection signal]),
    table.hline(),
    [kitty, ghostty], [bitmap (kitty)], [Terminal reply, then environment],
    [Konsole, Rio], [bitmap (kitty)], [Terminal reply, `KONSOLE_VERSION`, `TERM_PROGRAM=rio`],
    [iTerm2, WezTerm, mintty], [bitmap (OSC 1337)], [Terminal reply, `TERM_PROGRAM`, `LC_TERMINAL=iTerm2`],
    [Sixel terminals], [bitmap (Sixel)], [Terminal reply],
    [tmux with `allow-passthrough` on kitty or ghostty], [bitmap (kitty)], [tmux client report],
    [tmux on a Sixel terminal], [bitmap (Sixel)], [tmux client report],
    [other tmux, screen, mosh], [half-block], [`TMUX`, `STY`, or no graphics reply],
    [VS Code and other color terminals], [half-block], [no graphics reply],
    [`TERM` empty or `dumb`], [metadata text only], [`TERM`],
  )],
  caption: [TERMINAL COMPATIBILITY MATRIX],
  kind: table,
)

On Linux and macOS, QSoC asks the terminal for its graphics support, cell size,
and name when the agent starts, so a graphics terminal is recognized over SSH.
On Windows, set `QSOC_IMAGE_PROTOCOL` to use a bitmap protocol.
A second client in the same tmux session does not disable a graphics terminal's
bitmap previews.

== Folding and History
<tui-image-preview-fold>
Images stay in the scroll history and show again when you scroll back to
them. `Tab` on a focused image block folds it to its `[image: ...]` line.
A picture leaves room for the status and input lines on a short terminal.
A session keeps the newest 512 MiB of images in memory; older images keep
only their metadata line.

`/figures` lists every image in the session and scrolls to the selected one.
The name of a saved image is a link to its file on terminals with OSC 8 links.

== Selecting the Protocol
<tui-image-preview-gating>
`QSOC_IMAGE_PROTOCOL` selects the preview and overrides detection:
`kitty`, `iterm2`, `sixel`, `cells`, or `none`. `QSOC_NO_IMAGE_GRAPHICS` set to any
non-empty value shows metadata text only.

== Resize and Redraw
<tui-image-preview-resize>
An image that the viewport cuts off shows its visible part: kitty terminals
crop the bitmap, and OSC 1337 and Sixel terminals show the half-block preview
until the whole image fits.
