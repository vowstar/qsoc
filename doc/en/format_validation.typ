= Validation Tools and Features
<validation-format>
QSoC checks netlist connections during Verilog generation.

== Verilog Port Widths
<soc-net-verilog-widths>
Port width is `abs(msb - lsb) + 1`. For example, `output [7:3] signal` is 5 bits wide.

== Port Direction Checking
<soc-net-port-direction>
Top-level inputs drive internal logic; top-level outputs receive it.

=== Multiple Driver Detection
<soc-net-port-direction-drivers>
- Identifies nets with multiple output drivers that could cause conflicts
- Allows legitimate multiple drivers on non-overlapping bit ranges
- Allows legitimate multiple drivers under mutually exclusive `ifdef`/`ifndef`
  guards (see #emph[Macro guard exemption] below)
- Reports potential bus contention issues with detailed diagnostic information

==== Macro Guard Exemption
<soc-net-port-direction-drivers-macro>
Drivers guarded by opposite polarities of the same macro cannot be active
together. QSoC excludes these pairs from multiple-driver errors. Different
macros do not establish mutual exclusion; unguarded drivers remain checked.

==== Example: Tech-Portable Buffer
<soc-net-port-direction-drivers-tech-example>
```yaml
instance:
  u_clkbuf_fpga:
    module: clk_buf_fpga
    ifdef:  [TECH_FPGA]    # active iff TECH_FPGA defined
  u_clkbuf_asic:
    module: clk_buf_asic
    ifndef: [TECH_FPGA]    # active iff TECH_FPGA undefined

net:
  jtag_tck:
    - { instance: u_clkbuf_fpga, port: z }
    - { instance: u_clkbuf_asic, port: z }
    - { instance: top,           port: jtag_tck }
```

The opposite `TECH_FPGA` guards exclude a driver conflict on `jtag_tck`.
Using two unrelated macros does not exclude a conflict.

=== Undriven Net Detection
<soc-net-port-direction-undriven>
A net with only input ports has no driver and is reported as undriven.

== Bit-level Overlap Detection
<soc-net-bit-overlap>
=== Bit Range Analysis
<soc-net-bit-overlap-analysis>
- Analyzes bit selections like `[7:4]` and `[3:0]` for overlap detection
- Allows multiple drivers on non-overlapping bit ranges of the same net
- Detects conflicts when bit ranges overlap between different drivers

=== Supported Bit Selection Formats
<soc-net-bit-overlap-formats>
- Range selections: `signal[7:0]`, `signal[15:8]`
- Single bit selections: `signal[3]`, `signal[0]`
- Mixed range scenarios with proper overlap validation

=== Example Scenarios
<soc-net-bit-overlap-examples>
```yaml
# Valid: Non-overlapping bit ranges
net:
  data_bus:
    - { instance: cpu, port: data_out[7:4] }    # Upper nibble
    - { instance: mem, port: data_out[3:0] }    # Lower nibble

# Invalid: Overlapping bit ranges (will generate warning)
net:
  addr_bus:
    - { instance: cpu, port: addr_out[7:4] }    # Bits 7-4
    - { instance: dma, port: addr_out[5:2] }    # Bits 5-2 overlap with 5-4
```

== Validation Diagnostics
<soc-net-diagnostics>
QSoC provides detailed diagnostic information for all validation issues:

=== Error Reports
<soc-net-diagnostics-reports>
- Exact instance and port names involved in conflicts
- Bit range information for overlap detection
- Clear descriptions of the nature of each problem

=== Warning Categories
<soc-net-diagnostics-categories>
- `Multiple Drivers`: Multiple outputs driving the same net or overlapping bits
- `Undriven Nets`: Nets with no output drivers
- `Width Mismatches`: Port width incompatibilities

Direction problems are reported through the categories above: a port driven
from the wrong side shows up as `Multiple Drivers` or `Undriven Nets`, not as a
category of its own.

=== Integration with Generation Flow
<soc-net-diagnostics-integration>
- Validation occurs during Verilog generation process
- Issues are reported without preventing generation (when possible)

== Width Checking
<soc-net-width-checking>
QSoC performs automatic width checking for all connections:

+ It calculates the effective width of each port in a connection, considering bit selections
+ It compares widths of all ports connected to the same net
+ It generates warnings for width mismatches, including detailed information about port widths and bit selections

== Resolving Diagnostics

=== Common Issues and Solutions
<soc-net-validation-issues>

==== Multiple Drivers
Problem: Multiple outputs connected to the same net
Solution: Use bit selection to assign different bit ranges to different drivers, or use proper multiplexing logic

==== Undriven Nets
Problem: Net has only input connections, no driving source
Solution: Add appropriate output driver or tie signal to constant value

==== Width Mismatches
Problem: Connected ports have incompatible widths
Solution: Adjust port widths in module definitions or use bit selection for partial connections

==== Duplicate Connections
Problem: a net lists the same instance port twice, or one instance port is
wired into several nets
Solution: only the first connection is kept and the rest are dropped with a
warning. Remove the duplicates so the netlist says what the RTL does

== Generation-time Checks
<soc-net-generation-checks>
The checks above run on connectivity. The generators add their own, and some of
them change or drop parts of the design rather than only warning:

- *Missing instance port*: an unresolved port is instantiated as
  `.port(/* FIXME: ... missing */)`
- *`tie` width adaptation*: for a proven built-in port width, a literal is
  first bounded by its own declared width and then adapted to the port width;
  truncation emits a FIXME. Unknown or symbolic port widths preserve the
  literal without a guessed mask
- *Malformed `tie` values*: malformed numbers and macro-free expressions that
  fail SystemVerilog syntax are ignored with a warning, leaving the port in the
  unconnected report
- *`tie` on unsupported port kinds*: ignored with a warning; the port stays
  in the unconnected report
- *`invert` on an output destination*: reported as an error and generation is
  refused
- *Invalid identifiers*: port, parameter, and instance names must be IEEE
  1364-2001 simple identifiers, that is ASCII letters, digits, `_`, and `$`
  with no leading digit, and must not be a reserved keyword. Keyword matching
  is case-sensitive, so `Module` is a legal name and `module` is not
- *Artifact paths*: primary outputs whose resolved parent leaves `output/`
  are rejected. Writing through an existing symbolic link inside `output/` is
  allowed, so flows may redirect artifacts into their tree. Existing nested
  directories are allowed; optional diagrams, reports, and JSON sidecars
  remain non-critical
- *Bracket leakage in controller names*: reset, clock, and power configurations
  with brackets in a signal name are renamed, with a warning
- *Clock topology*: every target must be a map with a non-empty `link`, and
  controller, input, target, and link source names must be valid Verilog
  identifiers. A malformed configuration is reported and nothing is written.
  A link source that names neither an input nor another target becomes an
  input port on the controller
- *`connect:` aliases*: every spelling of a connected component is judged
  together; direction fixes each endpoint's role (top input sources, top
  output sinks) and declaration order only stabilizes equivalent sinks. The
  driver census covers bound inputs, instance outputs, and process targets:
  two drivers whose bit ranges overlap and whose `ifdef`/`ifndef` guards are
  not mutually exclusive are reported as an error and generation is refused.
  Inout endpoints stay out of the verdict and unwired; a component that
  actually fans out records a FIXME
- *Combinational and sequential driver conflicts*: a signal driven from more
  than one `comb`/`seq` block is rejected
- *Multiple outputs on one net*: two plain output ports owning overlapping
  bits of one net (with `ifdef`/`ifndef` guards that are not mutually
  exclusive) are reported as an error and generation is refused, on internal
  nets exactly as on top-level ones; declare a pin `inout` when the drivers
  are wired together. An output beside an `inout` warns: driving against a
  bidirectional pin is a contention hazard, but the `inout` side may
  legitimately never drive. A net carrying only `inout` pins is an ordinary
  bidirectional bus and is not reported
- *Power `follow` conflicts*: an entry whose `clock` equals `host_clock`, or
  whose `reset` equals `host_reset`, or that carries only one of the two, is
  reported as an error and generation is refused
- *Reset controller without `source`*: generation is refused outright. Without a
  source every target would be tied inactive and the system would never reset

The unconnected-port report (`<module>.nc.rpt`, see @verilog-generation) lists
every port left unconnected after all of the above.

== Known Limitations
<known-limitations>
These are reported here because generation succeeds and the output looks
ordinary. Check them by hand until they are closed.

- *Preprocessor-dependent `tie` expressions*: not syntax-checked because the
  caller's macro and include environment is unavailable; they otherwise follow
  the existing `tie` emission rules.
