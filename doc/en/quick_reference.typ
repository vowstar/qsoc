#heading(level: 1, numbering: none, outlined: false)[Quick Reference]
<quick-reference>

#[
  /* Plain key/value strips: no rules, no header shading */
  #set table(stroke: none, fill: none, inset: (x: 2pt, y: 4.2pt))
  #show table.cell: set text(weight: "regular")
  #set text(9pt)

  #let strip(title, note, key: 9.2em, ..rows) = block(breakable: false, width: 100%)[
    #text(10pt, weight: "bold")[#title] #h(0.4em) #text(8.5pt)[#note]
    #v(1pt)
    #table(columns: (key, 1fr), align: (left + top, left + top), ..rows)
    #v(10pt)
  ]

  #strip([First run], [@first-run], key: 14em,
    [`project create <name>`], [new project],
    [`module import -l <lib> *.v`], [import modules],
    [`generate verilog t.soc_net`], [`output/t/rtl/t.v`],
    [`agent`], [work by prompt],
  )

  #columns(2, gutter: 18pt)[
    #strip([Commands], [@cli-overview],
      [`project`], [`create` `update` `remove` `list` `show`],
      [`module`], [`import` `remove` `list` `show`],
      [], [`create` `validate` `bus`],
      [`bus`], [`import` `remove` `list` `show`],
      [`generate`], [`verilog` `module` `template` `stub`],
      [`gui`], [schematic, module, bus, PRC editors],
      [`agent`], [interactive agent],
    )

    #strip([Options you retype], [],
      [`-d, --directory`], [project directory],
      [`-p, --project`], [project name],
      [`-l, --library`], [module or bus library],
      [`-m, --merge`], [merge netlists in order],
      [`--with-formal`], [also write formal checks],
      [`--with-uvm`], [also write a UVM testbench],
      [`--format`], [run the Verible formatter],
      [`--verbose 0..5`], [silent … verbose],
      [`--color`], [`auto`, `always`, `never`],
    )

    #strip([Netlist sections], [@soc-net-format],
      [`port` `instance` `net`], [structure (@netlist-format)],
      [`bus`], [buses (@soc-net-bus)],
      [`comb` `seq` `fsm`], [behavior (@soc-net-comb)],
      [`reset` `clock` `power`], [controllers (@soc-net-reset-overview)],
    )

    #colbreak()

    #strip([Where things live], [@project-layout],
      [`<name>.soc_pro`], [project file],
      [`bus/` `module/`], [libraries],
      [`output/`], [netlists and generated units],
      [`.qsoc.yml`], [project configuration],
      [`.qsoc/`], [sessions, plans, skills, memory],
      [`~/.config/qsoc/`], [user configuration (@config-files)],
      [`QSOC_LLM_MODEL`], [model entry from the environment],
    )

    #strip([Agent], [@agent-commands],
      [`/help` `/status`], [what is loaded, and where],
      [`/model` `/effort`], [switch model or reasoning effort],
      [`/plan`], [read-only mode (*Shift+Tab*)],
      [`/clear` `/compact`], [reset or shrink the context],
      [`/resume [id]`], [switch to a saved session],
      [`/btw <question>`], [side question, not saved],
      [`/cwd` `/project`], [move the working root],
      [`/ssh` `/local`], [remote or local workspace],
      [`/memory` `#<fact>`], [inspect or add a memory],
      [`@<name>`], [complete a project file path],
    )

    #strip([Keys], [@agent-keyboard],
      [*ESC*], [cancel the running operation],
      [*Ctrl+J*], [newline in the prompt],
      [*Ctrl+R*], [search prompt history],
      [*Ctrl+X Ctrl+E*], [edit the prompt in `$EDITOR`],
      [*Ctrl+T* / *Ctrl+B*], [TODO list / background tasks],
    )
  ]
]
