// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoccodehighlighter.h"

#include <QSet>

namespace {

enum class Tok : std::uint8_t {
    Default,
    Keyword,
    String,
    Number,
    Comment,
    Preproc,
};

QTuiFgColor colorFor(Tok tok)
{
    switch (tok) {
    case Tok::Keyword:
        return QTuiFgColor::Magenta;
    case Tok::String:
        return QTuiFgColor::Green;
    case Tok::Number:
        return QTuiFgColor::Yellow;
    case Tok::Comment:
        return QTuiFgColor::Gray;
    case Tok::Preproc:
        return QTuiFgColor::Orange;
    case Tok::Default:
    default:
        return QTuiFgColor::Default;
    }
}

QTuiStyledRun makeRun(const QString &text, Tok tok)
{
    QTuiStyledRun run;
    run.text = text;
    /* Code lines stay dim by block convention; the highlight overlays
     * fg color on top so keywords pop without breaking the muted code
     * aesthetic. Comments stay extra-dim to read as background. */
    run.dim = true;
    run.fg  = colorFor(tok);
    return run;
}

/* Normalised language keyword sets. Sourced from each language's
 * canonical reserved-word list trimmed to the subset most useful for
 * scanning model-emitted code. Names are owned at file scope so
 * QSet construction happens once per process. */
const QSet<QString> &pythonKeywords()
{
    static const QSet<QString> kw = {
        "False", "None",     "True",  "and",    "as",   "assert", "async",  "await",    "break",
        "class", "continue", "def",   "del",    "elif", "else",   "except", "finally",  "for",
        "from",  "global",   "if",    "import", "in",   "is",     "lambda", "nonlocal", "not",
        "or",    "pass",     "raise", "return", "try",  "while",  "with",   "yield",
    };
    return kw;
}

const QSet<QString> &bashKeywords()
{
    static const QSet<QString> kw = {
        "if",       "then",  "else",   "elif",   "fi",       "case",  "esac",     "for",    "in",
        "while",    "until", "do",     "done",   "select",   "time",  "function", "return", "break",
        "continue", "exit",  "export", "local",  "readonly", "unset", "source",   "alias",  "let",
        "trap",     "shift", "echo",   "printf", "eval",     "set",   "unset",
    };
    return kw;
}

const QSet<QString> &cKeywords()
{
    static const QSet<QString> kw = {
        "auto",   "break",   "case",     "char",     "const",    "continue", "default",  "do",
        "double", "else",    "enum",     "extern",   "float",    "for",      "goto",     "if",
        "int",    "long",    "register", "return",   "short",    "signed",   "sizeof",   "static",
        "struct", "switch",  "typedef",  "union",    "unsigned", "void",     "volatile", "while",
        "_Bool",  "_Atomic", "inline",   "restrict", "_Alignof",
    };
    return kw;
}

const QSet<QString> &cppKeywords()
{
    static const QSet<QString> kw = []() {
        QSet<QString>     set   = cKeywords();
        const QStringList extra = {
            "alignas",       "alignof",     "and",       "asm",
            "bool",          "catch",       "class",     "constexpr",
            "const_cast",    "decltype",    "delete",    "dynamic_cast",
            "explicit",      "export",      "false",     "friend",
            "mutable",       "namespace",   "new",       "noexcept",
            "nullptr",       "operator",    "or",        "override",
            "private",       "protected",   "public",    "reinterpret_cast",
            "static_assert", "static_cast", "template",  "this",
            "thread_local",  "throw",       "true",      "try",
            "typeid",        "typename",    "using",     "virtual",
            "wchar_t",       "co_await",    "co_return", "co_yield",
            "concept",       "requires",
        };
        for (const auto &k : extra) {
            set.insert(k);
        }
        return set;
    }();
    return kw;
}

const QSet<QString> &verilogKeywords()
{
    static const QSet<QString> kw = {
        "module",      "endmodule", "input",     "output",       "inout",       "wire",
        "reg",         "logic",     "always",    "always_ff",    "always_comb", "always_latch",
        "initial",     "begin",     "end",       "if",           "else",        "case",
        "endcase",     "casex",     "casez",     "default",      "for",         "while",
        "do",          "repeat",    "forever",   "function",     "endfunction", "task",
        "endtask",     "return",    "assign",    "parameter",    "localparam",  "generate",
        "endgenerate", "genvar",    "integer",   "real",         "time",        "bit",
        "byte",        "shortint",  "int",       "longint",      "signed",      "unsigned",
        "typedef",     "struct",    "union",     "enum",         "package",     "endpackage",
        "import",      "export",    "interface", "endinterface", "modport",     "class",
        "endclass",    "extends",   "virtual",   "pure",         "rand",        "randc",
        "constraint",  "new",       "this",      "super",        "fork",        "join",
        "join_any",    "join_none", "wait",      "disable",      "force",       "release",
        "posedge",     "negedge",   "edge",      "or",           "and",         "xor",
    };
    return kw;
}

const QSet<QString> &jsonKeywords()
{
    static const QSet<QString> kw = {"true", "false", "null"};
    return kw;
}

const QSet<QString> &tclKeywords()
{
    static const QSet<QString> kw = {
        "proc",    "set",     "if",     "else",    "elseif",   "then",    "foreach",   "for",
        "while",   "do",      "return", "break",   "continue", "switch",  "expr",      "lindex",
        "llength", "lappend", "lset",   "list",    "dict",     "array",   "namespace", "package",
        "source",  "after",   "error",  "catch",   "try",      "throw",   "eval",      "exec",
        "format",  "regexp",  "regsub", "string",  "info",     "global",  "upvar",     "variable",
        "incr",    "append",  "puts",   "gets",    "open",     "close",   "read",      "write",
        "file",    "glob",    "scan",   "trace",   "subst",    "uplevel", "rename",    "unset",
        "concat",  "split",   "join",   "lsort",   "lreverse", "lrepeat", "lrange",    "lreplace",
        "linsert", "lassign", "lmap",   "lsearch",
    };
    return kw;
}

struct LexerSpec
{
    const QSet<QString> *keywords        = nullptr;
    bool                 hashComment     = false; /* `#` to EOL */
    bool                 slashComment    = false; /* `//` to EOL */
    bool                 blockComment    = false; /* slash-star block style */
    bool                 doubleQuoteStr  = false;
    bool                 singleQuoteStr  = false;
    bool                 cPreproc        = false; /* `#include`, `#define` */
    bool                 verilogLiteral  = false; /* 8'hFF, 4'b1010 */
    bool                 yamlKeyColoring = false; /* identifier-then-colon */
    bool                 isDiff          = false; /* per-line +/-/@@ */
};

const LexerSpec &lexerFor(const QString &language)
{
    static const LexerSpec defaultSpec{};
    static const LexerSpec python{
        .keywords        = &pythonKeywords(),
        .hashComment     = true,
        .slashComment    = false,
        .blockComment    = false,
        .doubleQuoteStr  = true,
        .singleQuoteStr  = true,
        .cPreproc        = false,
        .verilogLiteral  = false,
        .yamlKeyColoring = false,
        .isDiff          = false,
    };
    static const LexerSpec bash{
        .keywords        = &bashKeywords(),
        .hashComment     = true,
        .slashComment    = false,
        .blockComment    = false,
        .doubleQuoteStr  = true,
        .singleQuoteStr  = true,
        .cPreproc        = false,
        .verilogLiteral  = false,
        .yamlKeyColoring = false,
        .isDiff          = false,
    };
    static const LexerSpec cLang{
        .keywords        = &cKeywords(),
        .hashComment     = false,
        .slashComment    = true,
        .blockComment    = true,
        .doubleQuoteStr  = true,
        .singleQuoteStr  = true,
        .cPreproc        = true,
        .verilogLiteral  = false,
        .yamlKeyColoring = false,
        .isDiff          = false,
    };
    static const LexerSpec cppLang{
        .keywords        = &cppKeywords(),
        .hashComment     = false,
        .slashComment    = true,
        .blockComment    = true,
        .doubleQuoteStr  = true,
        .singleQuoteStr  = true,
        .cPreproc        = true,
        .verilogLiteral  = false,
        .yamlKeyColoring = false,
        .isDiff          = false,
    };
    static const LexerSpec verilog{
        .keywords        = &verilogKeywords(),
        .hashComment     = false,
        .slashComment    = true,
        .blockComment    = true,
        .doubleQuoteStr  = true,
        .singleQuoteStr  = false,
        .cPreproc        = false,
        .verilogLiteral  = true,
        .yamlKeyColoring = false,
        .isDiff          = false,
    };
    static const LexerSpec json{
        .keywords        = &jsonKeywords(),
        .hashComment     = false,
        .slashComment    = false,
        .blockComment    = false,
        .doubleQuoteStr  = true,
        .singleQuoteStr  = false,
        .cPreproc        = false,
        .verilogLiteral  = false,
        .yamlKeyColoring = false,
        .isDiff          = false,
    };
    static const LexerSpec tcl{
        .keywords        = &tclKeywords(),
        .hashComment     = true,
        .slashComment    = false,
        .blockComment    = false,
        .doubleQuoteStr  = true,
        .singleQuoteStr  = false,
        .cPreproc        = false,
        .verilogLiteral  = false,
        .yamlKeyColoring = false,
        .isDiff          = false,
    };
    static const LexerSpec yaml{
        .keywords        = nullptr,
        .hashComment     = true,
        .slashComment    = false,
        .blockComment    = false,
        .doubleQuoteStr  = true,
        .singleQuoteStr  = true,
        .cPreproc        = false,
        .verilogLiteral  = false,
        .yamlKeyColoring = true,
        .isDiff          = false,
    };
    static const LexerSpec diff{
        .keywords        = nullptr,
        .hashComment     = false,
        .slashComment    = false,
        .blockComment    = false,
        .doubleQuoteStr  = false,
        .singleQuoteStr  = false,
        .cPreproc        = false,
        .verilogLiteral  = false,
        .yamlKeyColoring = false,
        .isDiff          = true,
    };

    if (language == "python" || language == "py") {
        return python;
    }
    if (language == "bash" || language == "sh" || language == "shell" || language == "zsh") {
        return bash;
    }
    if (language == "c") {
        return cLang;
    }
    if (language == "cpp" || language == "c++" || language == "cxx" || language == "cc"
        || language == "hpp") {
        return cppLang;
    }
    if (language == "verilog" || language == "sv" || language == "systemverilog") {
        return verilog;
    }
    if (language == "json") {
        return json;
    }
    if (language == "yaml" || language == "yml") {
        return yaml;
    }
    if (language == "tcl" || language == "tk") {
        return tcl;
    }
    if (language == "diff" || language == "patch") {
        return diff;
    }
    return defaultSpec;
}

bool isIdentStart(QChar character)
{
    return character.isLetter() || character == QLatin1Char('_');
}

bool isIdentCont(QChar character)
{
    return character.isLetterOrNumber() || character == QLatin1Char('_');
}

/* Greedy quoted-string scan with backslash escape handling. Returns
 * the index just past the closing quote, or end-of-line if unmatched.
 * The unmatched case still produces a String token so the rest of the
 * line stays readable rather than being misclassified. */
int scanQuotedString(const QString &line, int start, QChar quote)
{
    int idx = start + 1;
    while (idx < line.size()) {
        const QChar character = line[idx];
        if (character == QLatin1Char('\\') && idx + 1 < line.size()) {
            idx += 2;
            continue;
        }
        if (character == quote) {
            return idx + 1;
        }
        ++idx;
    }
    return line.size();
}

/* Verilog-style sized literal: <decimal>?'(b|B|o|O|d|D|h|H)<digits>.
 * `pos` must point at the leading digit or apostrophe. Returns the
 * position past the literal, or -1 if the character isn't actually a
 * verilog literal start. */
int scanVerilogLiteral(const QString &line, int pos)
{
    int idx = pos;
    while (idx < line.size() && line[idx].isDigit()) {
        ++idx;
    }
    if (idx >= line.size() || line[idx] != QLatin1Char('\'')) {
        if (idx > pos) {
            return idx; /* plain decimal */
        }
        return -1;
    }
    ++idx; /* skip ' */
    if (idx >= line.size()) {
        return idx;
    }
    const QChar base = line[idx].toLower();
    if (base != QLatin1Char('b') && base != QLatin1Char('o') && base != QLatin1Char('d')
        && base != QLatin1Char('h')) {
        return idx;
    }
    ++idx;
    while (idx < line.size()
           && (line[idx].isLetterOrNumber() || line[idx] == QLatin1Char('_')
               || line[idx] == QLatin1Char('?') || line[idx] == QLatin1Char('x')
               || line[idx] == QLatin1Char('z'))) {
        ++idx;
    }
    return idx;
}

/* Merge adjacent runs that share styling so paint cost stays low and
 * the styled-run list mirrors what a human would group by eye. */
QList<QTuiStyledRun> mergeAdjacent(QList<QTuiStyledRun> runs)
{
    QList<QTuiStyledRun> out;
    for (const auto &run : runs) {
        if (run.text.isEmpty()) {
            continue;
        }
        if (!out.isEmpty() && out.last().bold == run.bold && out.last().italic == run.italic
            && out.last().dim == run.dim && out.last().underline == run.underline
            && out.last().fg == run.fg && out.last().bg == run.bg) {
            out.last().text.append(run.text);
        } else {
            out.append(run);
        }
    }
    return out;
}

} // namespace

QList<QTuiStyledRun> QSocCodeHighlighter::highlight(const QString &line, const QString &language)
{
    if (line.isEmpty()) {
        return {};
    }
    const LexerSpec &spec = lexerFor(language);

    /* Diff is rendered per-line based on the leading marker. Skip the
     * token-level pipeline and apply a single line color so wrapped
     * patches keep reading as +/- columns. */
    if (spec.isDiff) {
        Tok tok = Tok::Default;
        if (line.startsWith(QLatin1Char('+')) && !line.startsWith(QStringLiteral("+++"))) {
            tok = Tok::String; /* green for additions */
        } else if (line.startsWith(QLatin1Char('-')) && !line.startsWith(QStringLiteral("---"))) {
            tok = Tok::Number; /* repurposed: yellow on red would clash; use accent */
            /* Use a dedicated red via Number? We don't expose Red here.
             * Fall through to comment styling for "removed" so it reads
             * dimmer. */
            tok = Tok::Comment;
        } else if (line.startsWith(QStringLiteral("@@"))) {
            tok = Tok::Keyword;
        }
        return {makeRun(line, tok)};
    }

    /* C / C++ preprocessor lines (`#include`, `#define`) light up the
     * leading word. Detected before identifier scan so the leading `#`
     * doesn't interfere with the `#`-as-comment path of other langs. */
    if (spec.cPreproc) {
        int idx = 0;
        while (idx < line.size() && line[idx].isSpace()) {
            ++idx;
        }
        if (idx < line.size() && line[idx] == QLatin1Char('#')) {
            return {makeRun(line, Tok::Preproc)};
        }
    }

    QList<QTuiStyledRun> runs;
    int                  pos = 0;
    while (pos < line.size()) {
        const QChar character = line[pos];

        /* Single-line comments swallow the rest of the line. */
        if (spec.hashComment && character == QLatin1Char('#')) {
            runs.append(makeRun(line.mid(pos), Tok::Comment));
            return mergeAdjacent(runs);
        }
        if (spec.slashComment && character == QLatin1Char('/') && pos + 1 < line.size()
            && line[pos + 1] == QLatin1Char('/')) {
            runs.append(makeRun(line.mid(pos), Tok::Comment));
            return mergeAdjacent(runs);
        }

        /* Single-line slice of a block comment: only highlights what
         * is on the current line; cross-line state is the caller's
         * problem. */
        if (spec.blockComment && character == QLatin1Char('/') && pos + 1 < line.size()
            && line[pos + 1] == QLatin1Char('*')) {
            int idx = pos + 2;
            while (idx + 1 < line.size()
                   && !(line[idx] == QLatin1Char('*') && line[idx + 1] == QLatin1Char('/'))) {
                ++idx;
            }
            const int end = (idx + 1 < line.size()) ? idx + 2 : line.size();
            runs.append(makeRun(line.mid(pos, end - pos), Tok::Comment));
            pos = end;
            continue;
        }

        /* Double / single quoted strings. */
        if (spec.doubleQuoteStr && character == QLatin1Char('"')) {
            const int end = scanQuotedString(line, pos, QLatin1Char('"'));
            runs.append(makeRun(line.mid(pos, end - pos), Tok::String));
            pos = end;
            continue;
        }
        if (spec.singleQuoteStr && character == QLatin1Char('\'')) {
            const int end = scanQuotedString(line, pos, QLatin1Char('\''));
            runs.append(makeRun(line.mid(pos, end - pos), Tok::String));
            pos = end;
            continue;
        }

        /* Verilog literals: digits possibly followed by '<base><value>. */
        if (spec.verilogLiteral && character.isDigit()) {
            const int end = scanVerilogLiteral(line, pos);
            if (end > pos) {
                runs.append(makeRun(line.mid(pos, end - pos), Tok::Number));
                pos = end;
                continue;
            }
        }

        /* Plain numeric literal: decimal or hex. */
        if (character.isDigit()) {
            int idx = pos + 1;
            if (character == QLatin1Char('0') && idx < line.size()
                && (line[idx] == QLatin1Char('x') || line[idx] == QLatin1Char('X'))) {
                ++idx;
                while (idx < line.size()
                       && (line[idx].isDigit()
                           || (line[idx].toLower() >= QLatin1Char('a')
                               && line[idx].toLower() <= QLatin1Char('f')))) {
                    ++idx;
                }
            } else {
                while (idx < line.size()
                       && (line[idx].isDigit() || line[idx] == QLatin1Char('.')
                           || line[idx] == QLatin1Char('_'))) {
                    ++idx;
                }
            }
            runs.append(makeRun(line.mid(pos, idx - pos), Tok::Number));
            pos = idx;
            continue;
        }

        /* Identifier: keyword lookup, yaml key heuristic. */
        if (isIdentStart(character)) {
            int idx = pos + 1;
            while (idx < line.size() && isIdentCont(line[idx])) {
                ++idx;
            }
            const QString word = line.mid(pos, idx - pos);
            Tok           tok  = Tok::Default;
            if (spec.keywords != nullptr && spec.keywords->contains(word)) {
                tok = Tok::Keyword;
            } else if (spec.yamlKeyColoring) {
                int trail = idx;
                while (trail < line.size() && line[trail].isSpace()) {
                    ++trail;
                }
                if (trail < line.size() && line[trail] == QLatin1Char(':')) {
                    tok = Tok::Keyword;
                }
            }
            runs.append(makeRun(word, tok));
            pos = idx;
            continue;
        }

        /* Anything else (punctuation, whitespace, operators) is left
         * at default fg so prose reads naturally. */
        runs.append(makeRun(QString(character), Tok::Default));
        ++pos;
    }
    return mergeAdjacent(runs);
}
