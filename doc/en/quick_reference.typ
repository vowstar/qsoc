#heading(level: 1, numbering: none, outlined: false)[Quick Reference]
<quick-reference>

#[
  /* Plain key/value strips: no rules, no header shading */
  #set table(stroke: none, fill: none, inset: (x: 2pt, y: 1.6pt))
  #show table.cell: set text(weight: "regular")
  #set text(9pt)

  #let strip(title, note, ..rows) = block(breakable: false, width: 100%)[
    #text(9.5pt, weight: "bold")[#title] #h(0.4em) #text(8pt)[#note]
    #v(1pt)
    #table(columns: (auto, 1fr), align: (left + top, left + top), ..rows)
    #v(5pt)
  ]

  #columns(2, gutter: 18pt)[
    #strip([First build], [@getting-started],
      [`chmod +x QSoC-*.AppImage`], [make it runnable],
      [`qsoc project create <name>`], [create a project],
      [`qsoc module import rtl/*.v`], [import modules],
      [`qsoc generate verilog f.soc_net`], [write `output/f.v`],
      [`qsoc agent`], [drive the same tools by prompt],
    )

    #strip([Commands], [@cli-overview],
      [`project`], [`create` `update` `remove` `list` `show`],
      [`module`], [`import` `remove` `list` `show` `bus`],
      [`bus`], [`import` `remove` `list` `show`],
      [`generate`], [`verilog` `template` `stub`],
      [`gui`], [schematic, module, bus, PRC editors],
      [`agent`], [interactive agent],
    )

    #strip([Options you retype], [],
      [`-d, --directory`], [project directory],
      [`-p, --project`], [project name],
      [`-l, --library`], [module or bus library],
      [`-m, --merge`], [fold netlists; first name wins],
      [`--format`], [post-process with `verible-verilog-format`],
      [`--verbose 0..5`], [silent … verbose],
      [`--color`], [`auto`, `always`, `never`],
    )

    #colbreak()

    #strip([Where things live], [@project-layout],
      [`<name>.soc_pro`], [project file],
      [`bus/` `module/`], [libraries],
      [`output/`], [netlists in, `.v` and `.nc.rpt` out],
      [`.qsoc.yml`], [project configuration],
      [`.qsoc/`], [sessions, plans, skills, memory],
      [`~/.config/qsoc/`], [user configuration (@config-files)],
      [`QSOC_LLM_*`], [endpoint from the environment],
    )

    #strip([Agent], [@agent-commands],
      [`/help` `/status`], [what is loaded, and where],
      [`/model` `/effort`], [switch model or reasoning effort],
      [`/plan`], [read-only mode (*Shift+Tab*)],
      [`/clear` `/compact`], [reset or shrink the context],
      [`/cwd` `/project`], [move the working root],
      [`/ssh` `/local`], [remote or local workspace],
      [`/memory` `#<fact>`], [inspect or add a memory],
      [`@<name>`], [complete a project file path],
    )

    #strip([Keys], [@agent-keyboard],
      [*ESC*], [cancel the running operation],
      [*Ctrl+J*], [newline (*Shift+Enter* where supported)],
      [*Ctrl+R*], [search prompt history],
      [*Ctrl+X Ctrl+E*], [edit the prompt in `$EDITOR`],
      [*Ctrl+T* / *Ctrl+B*], [TODO list / background tasks],
    )

    #strip([Netlist sections], [@soc-net-format],
      [`port` `instance` `net`], [structure (@netlist-format)],
      [`bus`], [protocol connections (@soc-net-bus)],
      [`comb` `seq` `fsm`], [behavior (@soc-net-comb)],
      [`reset` `clock` `power`], [controllers (@soc-net-reset-overview)],
    )
  ]
]
