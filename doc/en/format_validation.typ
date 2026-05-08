= VALIDATION TOOLS AND FEATURES
<validation-format>
QSoC includes comprehensive netlist validation capabilities to ensure design integrity and catch potential issues early in the design process.

== PROCESSING FLOW
<soc-net-processing>
When QSoC processes a SOC_NET file, it follows this sequence:

1. Parse all module definitions referenced in the instance section
2. Validate port connections against module definitions
3. Process `link` and `uplink` attributes to generate nets and top-level ports
4. Expand bus connections into individual nets based on bus interface definitions
5. Process and validate combinational logic (`comb`) section
6. Process and validate sequential logic (`seq`) section
7. Process and validate finite state machine (`fsm`) section
8. Calculate effective widths for all connections, considering bit selections
9. Check for width mismatches and generate appropriate warnings
10. Generate Verilog output based on the processed netlist

The Verilog generation follows this structure:
1. Module declaration with ports and parameters
2. Wire declarations (from processed nets)
3. Module instantiations (from instance section)
4. Combinational logic blocks (from comb section)
5. Sequential logic blocks (from seq section)
6. Finite state machine blocks (from fsm section)
7. Module termination

== VERILOG PORT WIDTHS
<soc-net-verilog-widths>
QSoC correctly handles Verilog port width declarations where LSB is not zero. For example, a port declared as `output [7:3] signal` in Verilog has a width of 5 bits. The SOC_NET format and processing logic properly calculates this width as `|7-3|+1 = 5`. This ensures accurate width checking even with non-zero-based bit ranges.

== PORT DIRECTION CHECKING
<soc-net-port-direction>
The netlist processor performs sophisticated port direction validation to detect connectivity issues:

=== Top-Level Port Handling
<soc-net-port-direction-toplevel>
- Correctly recognizes that top-level `input` ports should drive internal logic
- Correctly recognizes that top-level `output` ports should be driven by internal logic
- Prevents false warnings about top-level port direction conflicts
- Properly handles bidirectional (`inout`) top-level ports

=== Multiple Driver Detection
<soc-net-port-direction-drivers>
- Identifies nets with multiple output drivers that could cause conflicts
- Allows legitimate multiple drivers on non-overlapping bit ranges
- Allows legitimate multiple drivers under mutually exclusive `ifdef`/`ifndef`
  guards (see #emph[Macro guard exemption] below)
- Reports potential bus contention issues with detailed diagnostic information

==== Macro Guard Exemption
<soc-net-port-direction-drivers-macro>
A common pattern in technology-portable RTL is two competing drivers selected
by `ifdef`/`ifndef` of a tech-flag macro (FPGA buffer vs. ASIC buffer, sim
shim vs. silicon cell, etc.). After preprocessing, only one driver is
present per build configuration, so the net is single-driven; flagging it
as a conflict produces a false positive.

QSoC models each instance's `ifdef` / `ifndef` lists as a Boolean cube
(conjunction of literals over macro symbols):

$ C = (and.big_(m in "ifdef") +m) and (and.big_(m in "ifndef") not m) $

Two drivers cannot be simultaneously active when their cubes are disjoint.
For cubes built from `ifdef` / `ifndef` lists, this reduces to a
constant-time test: cubes $C_1$ and $C_2$ are disjoint iff some macro
appears positively in one driver and negatively in the other, that is

$ exists m: (m in "ifdef"_1 and m in "ifndef"_2)
            or (m in "ifndef"_1 and m in "ifdef"_2). $

This is the standard cube-disjointness test from Boolean cube algebra (the
same primitive used in classical logic minimization). It is decidable in
$O(|C_1| + |C_2|)$ via hash-set lookup. No SAT/SMT solver is required, and
no false positives arise from `defined(X)` versus `not "defined"(X)` of the
same macro.

Empty guards represent the universal cube (driver is always active) and
are never disjoint with any other cube, so unguarded drivers continue to be
checked unchanged.

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

The two drivers carry the cubes $C_1 = +"TECH_FPGA"$ and
$C_2 = not "TECH_FPGA"$. The polarity collision on `TECH_FPGA` proves them
disjoint, so QSoC suppresses the multi-driver `FIXME` for `jtag_tck`.

If both drivers were guarded by different macros (for example
`ifdef HAS_FPGA_CLKBUF` and `ifdef HAS_ASIC_CLKBUF`), there is no polarity
collision and both could be defined simultaneously; QSoC keeps the warning
in this conservative case, as it should.

=== Undriven Net Detection
<soc-net-port-direction-undriven>
- Identifies nets that have no driving source (all input ports)
- Helps catch incomplete connections and missing driver assignments
- Provides clear error messages indicating which nets need attention

== BIT-LEVEL OVERLAP DETECTION
<soc-net-bit-overlap>
Advanced bit-level analysis prevents conflicts in multi-driver scenarios:

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

== VALIDATION DIAGNOSTICS
<soc-net-diagnostics>
QSoC provides detailed diagnostic information for all validation issues:

=== Comprehensive Error Reports
<soc-net-diagnostics-reports>
- Exact instance and port names involved in conflicts
- Bit range information for overlap detection
- Clear descriptions of the nature of each problem
- Suggestions for resolving connectivity issues

=== Warning Categories
<soc-net-diagnostics-categories>
- `Multiple Drivers`: Multiple outputs driving the same net or overlapping bits
- `Undriven Nets`: Nets with no output drivers
- `Width Mismatches`: Port width incompatibilities
- `Direction Conflicts`: Improper port direction usage

=== Integration with Generation Flow
<soc-net-diagnostics-integration>
- Validation occurs during Verilog generation process
- Issues are reported without preventing generation (when possible)
- Allows iterative design refinement with immediate feedback

== WIDTH CHECKING
<soc-net-width-checking>
QSoC performs automatic width checking for all connections:

1. It calculates the effective width of each port in a connection, considering bit selections
2. It compares widths of all ports connected to the same net
3. It generates warnings for width mismatches, including detailed information about port widths and bit selections

This automatic checking helps catch design errors early in the development process and ensures signal integrity across the design hierarchy.

== BEST PRACTICES FOR VALIDATION
<soc-net-validation-practices>

=== Design Guidelines
<soc-net-validation-design-guidelines>
- Always specify complete port connections to avoid undriven nets
- Use bit selection carefully to prevent overlapping drivers
- Verify port directions match the intended data flow
- Check width compatibility between connected ports

=== Debugging Tips
<soc-net-validation-debugging>
- Review validation warnings systematically
- Use descriptive names for nets and instances to aid debugging
- Test complex bit selection patterns incrementally
- Verify module definitions match actual usage

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

==== Direction Conflicts
Problem: Port directions don't match connectivity requirements
Solution: Review module definitions and fix port directions to match intended signal flow
