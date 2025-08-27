= TEMPLATE FORMAT
<template-format>
QSoC provides a powerful template system based on the Inja template engine for generating custom files from structured data sources including CSV, YAML, JSON, and SystemRDL files.

== OVERVIEW
<template-overview>
The template system allows users to create dynamic output files by combining template files with data sources. Templates use Inja syntax for variable substitution, loops, conditions, and advanced text processing.

=== Supported Data Sources
<template-data-sources>
#figure(
  align(center)[#table(
    columns: (0.15fr, 1fr),
    align: (auto, left),
    table.header([Format], [Description]),
    table.hline(),
    [CSV], [Comma/semicolon separated values with automatic type detection],
    [YAML], [YAML files with deep merging support],
    [JSON], [JSON objects with deep merging support],
    [RDL], [SystemRDL register description files],
    [RCSV], [SystemRDL CSV format for register definitions],
  )],
  caption: [SUPPORTED DATA SOURCES],
  kind: table,
)

=== Template Engine
<template-engine>
QSoC uses the Inja template engine, which provides:
- Variable substitution: `{{ variable }}`
- Control structures: `{% if condition %}...{% endif %}`
- Loops: `{% for item in list %}...{% endfor %}`
- Filters: `{{ value | filter_name(args) }}`
- Comments: `{# This is a comment #}`

== REGEX FILTERS
<regex-filters>
QSoC provides three powerful regex filters for text processing within templates. All filters support inline modifiers for pattern matching options.

=== regex_search
<regex-search>
Returns the first match or a default value if no match is found.

*Syntax:* `{{ value | regex_search(pattern, group, defaultVal) }}`

*Parameters:*
- `value`: Input string to search
- `pattern`: Regular expression pattern (supports inline modifiers)
- `group`: Capture group number (0=whole match, 1-N=capture groups, default: 0)
- `defaultVal`: Value to return if no match found (default: "")

*Examples:*
````
{# Extract first ID number #}
{{ "ID:123 NAME:John ID:456" | regex_search("ID:(\\d+)", 1) }}
{# Result: "123" #}

{# Case insensitive with default #}
{{ "Hello World" | regex_search("(?i)goodbye", 0, "NOT_FOUND") }}
{# Result: "NOT_FOUND" #}

{# Extract email domain #}
{{ "user@example.com" | regex_search("@([^.]+)", 1, "unknown") }}
{# Result: "example" #}
````

=== regex_findall
<regex-findall>
Returns all matches as an array.

*Syntax:* `{{ value | regex_findall(pattern, group) }}`

*Parameters:*
- `value`: Input string to search
- `pattern`: Regular expression pattern (supports inline modifiers)
- `group`: Capture group number (0=whole match, 1-N=capture groups, default: 0)

*Examples:*
````
{# Find all numbers #}
{% for num in "Price: $123, Tax: $45, Total: $168" | regex_findall("\\$(\\d+)", 1) %}
- {{ num }}
{% endfor %}

{# Find all words (case insensitive) #}
{% for word in "Error ERROR error" | regex_findall("(?i)error") %}
- {{ word }}
{% endfor %}

{# Extract register names #}
{% for reg in code_text | regex_findall("REG_\\w+") %}
#define {{ reg }}_OFFSET ...
{% endfor %}
````

=== regex_replace
<regex-replace>
Replaces all matching patterns with replacement text.

*Syntax:* `{{ value | regex_replace(pattern, replacement) }}`

*Parameters:*
- `value`: Input string to process
- `pattern`: Regular expression pattern (supports inline modifiers)
- `replacement`: Replacement string (supports backreferences \\1, \\2, etc.)

*Examples:*
````
{# Replace whitespace with underscores #}
{{ "hello world test" | regex_replace("\\s+", "_") }}
{# Result: "hello_world_test" #}

{# Swap parts using backreferences #}
{{ "ABC123DEF456" | regex_replace("([A-Z]+)(\\d+)", "\\2-\\1") }}
{# Result: "123-ABC456-DEF" #}

{# Case insensitive replacement #}
{{ "Error ERROR error" | regex_replace("(?i)error", "WARNING") }}
{# Result: "WARNING WARNING WARNING" #}
````

=== Inline Modifiers
<inline-modifiers>
Use inline modifiers instead of separate parameters:

- `(?i)` - Case insensitive matching
- `(?m)` - Multiline mode (\^ and \$ match line boundaries)
- `(?s)` - Dotall mode (. matches newlines)
- `(?i:...)` - Local case insensitive for group only

*Examples:*
````
{# Case insensitive #}
{{ text | regex_search("(?i)error") }}

{# Multiple modifiers #}
{{ text | regex_search("(?im)^error.*$") }}

{# Local modifier #}
{{ text | regex_search("name:(?i:[a-z]+)") }}
````

== DATA ACCESS PATTERNS
<data-access-patterns>
Template data is organized based on input file types and can be accessed using standard Inja syntax.

=== CSV Data Access
<csv-data-access>
CSV files are loaded as arrays of objects, with automatic type detection for numbers.

```yaml
# For CSV file: registers.csv
# name,address,width,description
# CTRL,0x1000,32,Control register
# STATUS,0x1004,32,Status register
```

````
{# Access by filename (without extension) #}
{% for reg in registers %}
#define {{ reg.name }}_ADDR {{ reg.address | format("0x{:08X}") }}
{% endfor %}

{# Access global data array (all CSV rows) #}
{% for item in data %}
{# Process all CSV data from all files #}
{% endfor %}
````

=== Combined Example
<combined-example>
Template combining regex filters with CSV data:

````
{# Auto-generated register definitions #}
{% for reg in registers %}
/* {{ reg.description }} */
#define {{ reg.name | regex_replace("^REG_", "") }}_ADDR {{ reg.address }}

{% endfor %}

{# Register name list #}
{% for name in data | tojson | regex_findall("\"name\"\\s*:\\s*\"([^\"]+)\"", 1) %}
    "{{ name }}",
{% endfor %}
````

== COMMAND USAGE
<command-usage>
Templates are processed using the `qsoc generate template` command:

```bash
# Basic template rendering
qsoc generate template input.j2 output.txt --csv data.csv

# Multiple data sources
qsoc generate template template.j2 result.h \
  --csv registers.csv \
  --yaml config.yaml \
  --json metadata.json

# SystemRDL integration
qsoc generate template template.j2 output.sv \
  --rdl registers.rdl \
  --rcsv register_defs.csv
```

== BEST PRACTICES
<best-practices>
1. *Use inline modifiers* instead of separate parameters for regex operations
2. *Validate regex patterns* during template development
3. *Handle missing data* gracefully using default values in regex_search
4. *Combine filters* for complex text transformations
5. *Comment templates* using `{# ... #}` for maintainability
6. *Test with sample data* before production use
