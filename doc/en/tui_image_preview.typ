= TUI LIVE IMAGE PREVIEW
<tui-image-preview>
The agent TUI shows bitmap attachments (PNG, JPG, GIF, WebP) inline
on terminals that speak the kitty graphics or iTerm2 inline image
protocol; other terminals see the `[image: ...]` metadata only.

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
    [tmux / screen], [placeholder text only], [`TMUX` or `STY` set],
    [other], [placeholder text only], [no signal],
  )]
)

== AUTO-FOLD ON NEW IMAGE
<tui-image-preview-fold>
Only the latest image renders as a bitmap; older previews collapse
to their `[image: ...]` line. `Tab` on a focused image block toggles
the fold state.

== MULTIPLEXER AND OPT-OUT GATING
<tui-image-preview-gating>
Image graphics are suppressed when `TMUX` or `STY` is set
(multiplexers strip the escapes) or when `QSOC_NO_IMAGE_GRAPHICS` is
set to any non-empty value.
