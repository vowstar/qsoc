= About This Guide
<about>
This guide documents the QSoC version in the page header. Start with the row
that fits you:

#figure(
  align(center)[#table(
    columns: (auto, 1fr),
    align: (left, left),
    table.header([You want to], [Read]),
    table.hline(),
    [See what QSoC does and which chapter covers each task], [@overview],
    [Install it and generate a first top level], [@getting-started, @first-run],
    [Look up a command, option, or key], [Quick Reference],
    [Use the terminal agent], [@agent-setup, @agent-overview],
    [Use the GUI], [@gui-overview],
    [Upgrade from an earlier version], [@upgrading],
  )],
  caption: [WHERE TO START],
  kind: table,
)

== Upgrading
<upgrading>
These releases changed files or options that existing projects use:

#figure(
  align(center)[#table(
    columns: (auto, 1fr, auto),
    align: (left, left, left),
    table.header([From], [Change], [See]),
    table.hline(),
    [2.5.1 or earlier],
    [Generated files moved into `output/<unit>/rtl/`, cells into
     `output/qsoc_cell/`, and `generate verilog --force` is removed],
    [@output-upgrade],
    [1.x],
    [`llm.url`, `llm.key`, `llm.model_reasoning`, `QSOC_LLM_URL`,
     `QSOC_LLM_KEY`, and `--model-reasoning` are removed. Declare each model
     under `llm.models`],
    [@llm-config],
  )],
  caption: [UPGRADE NOTES],
  kind: table,
)

Per-release changes, binaries, and the matching PDF are at
#link("https://github.com/vowstar/qsoc/releases").

== Disclaimer
<disclaimer>
Information in this document, including URL references, is subject to change
without notice. *This document is provided as is with no warranties whatsoever,
including any warranty of merchantability, non-infringement, fitness for any
particular purpose, or any warranty otherwise arising out of any proposal,
specification or sample.*

All liability, including liability for infringement of any proprietary rights,
relating to use of information in this document is disclaimed. No licenses
express or implied, by estoppel or otherwise, to any intellectual property
rights are granted herein.

All trade names, trademarks, and registered trademarks mentioned in this
document are property of their respective owners, and are hereby acknowledged.
