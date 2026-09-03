#!/usr/bin/env python3
"""check_script_flags.py -- every --flag a shipped script passes to an E555
binary must be one that binary's argument parser accepts.

"Script" means any runner in pipeline/, examples/ or tests/, in shell or in
Python. The two are read differently -- Python statements come from the
tokenizer, and its flags are string literals where a shell script's are bare --
but they drift from the binaries in exactly the same way.

`bash -n` proves a script parses, not that it can run: when --gumbel_tau0 and
--gumbel_tau1 were removed from the beamer, two pipelines kept passing them and
went on parsing perfectly. The binaries reject an unknown flag at startup, so
the failure lands on whoever runs the pipeline next, months later.

The accepted set is harvested from the parsers themselves -- the strcmp(argv[i],
"--x") sites -- so it cannot drift from the code the way a hand-kept list would.

Attribution is deliberately strict: a flag is checked only when it can be tied
to one binary, either by sharing a shell statement with it or by sitting in an
argument array that statement expands. Everything else is left alone, because
these scripts also drive the Python tools, whose flags are none of this
check's business. A missed flag is a check that did not fire; a false alarm is
a gate nobody can get past.

It also checks that each tool's --print_cmd printer covers every flag its own
parser accepts. That printer is hand-written, so it falls behind the parser
within a couple of commits and starts emitting a command line that does not
reproduce the run -- a silent lie, and worse than no feature. The check is
static: it reads the "--x" literals out of the print_cmd() function body rather
than running the binary, so mutually exclusive flags (--dedup/--no_dedup) and
conditional ones (--clue_center) count as covered by being mentioned.

It also refuses a comment placed between two backslash-continued lines. That
is valid shell and `bash -n` accepts it, but the comment swallows the rest of
the line: the command is cut short at that point and every remaining argument
line runs as a command of its own. Silent, and it truncates the invocation it
was meant to explain.

    python3 tests/check_script_flags.py            # every shipped script
    python3 tests/check_script_flags.py --verbose  # plus per-tool tallies
"""
import io
import os
import re
import sys
import tokenize

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Each tool's parser, plus any shared translation unit that also reads argv.
TOOL_SOURCES = {
    "E555_beamer":      ["src/B_beam/E555_beamer.c",     "src/B_beam/E555_database.c"],
    "E555_finalizer":   ["src/B_beam/E555_finalizer.c",  "src/B_beam/E555_database.c"],
    "E555_roundhouse":  ["src/B_beam/E555_roundhouse.c", "src/B_beam/E555_database.c"],
    "E555_backtracker": ["src/C_tail/E555_backtracker.c"],
}

SCRIPT_DIRS = ["examples", "pipeline", "tests"]

FLAG_RE = re.compile(r'(?<![\w"\'-])(--[A-Za-z][A-Za-z0-9_-]*)')
# In shell a flag is bare, and a quoted one is prose to be skipped. In Python
# it is the other way round: every flag IS a string literal, and bare text is
# prose. Same job, opposite quoting -- so the two need different patterns.
PY_FLAG_RE = re.compile(r'["\'](--[A-Za-z][A-Za-z0-9_-]*)["\']')
PARSE_RE = re.compile(r'strn?cmp\s*\([^,]+,\s*"(--[A-Za-z0-9_-]+)"')
# Only a path into bin/ is a binary. tools/E555_rank.py must not match.
TOOL_RE = re.compile(r'bin/(E555_[A-Za-z_]+)')
# NAME=( ... ) and NAME+=( ... ), the two ways these scripts build argument lists
ARRAY_RE = re.compile(r'^\s*([A-Za-z_][A-Za-z0-9_]*)\+?=\(')
# The Python spelling of the same thing: geom = ["--rounds", 3, ...], built in
# one statement and passed to a binary in another.
PY_ARRAY_RE = re.compile(r'^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*\[')
EXPAND_RE = re.compile(r'\$\{([A-Za-z_][A-Za-z0-9_]*)\[@\]\}')


def accepted_flags():
    """Harvest each tool's accepted flags from its own parser."""
    out = {}
    for tool, sources in TOOL_SOURCES.items():
        flags = set()
        for rel in sources:
            path = os.path.join(REPO, rel)
            if not os.path.exists(path):
                sys.exit("missing source: %s (has the tree moved?)" % rel)
            with open(path, encoding="utf-8", errors="replace") as fh:
                flags.update(m.group(1) for m in PARSE_RE.finditer(fh.read()))
        if not flags:
            sys.exit("no --flags found in %s's parser -- has the parser style "
                     "changed? This check would silently pass everything." % tool)
        out[tool] = flags
    return out


HEREDOC_RE = re.compile(r"<<-?\s*([\"\']?)([A-Za-z_][A-Za-z0-9_]*)\1")


class _Scanner:
    """Walks a script keeping quote and heredoc state ACROSS lines.

    Line-at-a-time quote tracking desynchronises on the multi-line awk programs
    these scripts are full of: the opening quote on one line and its close on
    the next look like two separate spans, after which every paren inside the
    program leaks into the statement depth and the scan swallows the rest of
    the function.
    """

    def __init__(self):
        self.quote = None        # ' or " while inside a quoted span
        self.heredoc = None      # terminator tag while inside a heredoc body

    def scan(self, line):
        """Return (code_outside_quotes, in_heredoc) for one raw line."""
        if self.heredoc is not None:
            if line.strip() == self.heredoc:
                self.heredoc = None
            return "", True

        out, i, n = [], 0, len(line)
        while i < n:
            ch = line[i]
            if self.quote:
                out.append(" ")
                if ch == self.quote:
                    self.quote = None
                elif ch == "\\" and self.quote == '"' and i + 1 < n:
                    out.append(" ")
                    i += 1
            elif ch in "\"'":
                self.quote = ch
                out.append(" ")
            elif ch == "#" and (i == 0 or line[i - 1] in " \t"):
                break
            else:
                out.append(ch)
            i += 1

        code = "".join(out)
        m = HEREDOC_RE.search(code)
        if m and self.quote is None:
            self.heredoc = m.group(2)
        return code, False


def statement_blocks(lines):
    """Yield (first_line_no, [(line_no, text)]) for each shell statement.

    A statement runs on past a trailing backslash and past an unclosed paren,
    which is how the pipelines build BEAM_CMD=( ... ) across five lines. Lines
    inside a heredoc carry no flags and are dropped.
    """
    sc = _Scanner()
    i, n = 0, len(lines)
    while i < n:
        start, block, depth = i, [], 0
        while i < n:
            code, in_heredoc = sc.scan(lines[i])
            if not in_heredoc:
                # flags are read from the real text, minus any trailing comment
                text = lines[i][:len(code)] if len(code) < len(lines[i]) else lines[i]
                block.append((i + 1, text))
                depth += code.count("(") - code.count(")")
            cont = code.rstrip().endswith("\\")
            i += 1
            if not cont and depth <= 0 and sc.quote is None and sc.heredoc is None:
                break
        if block:
            yield start + 1, block


def python_blocks(lines):
    """Yield (first_line_no, [(line_no, text)]) for each Python statement.

    Logical lines come from the tokenizer, so a bracketed call spanning six
    lines is one statement and the binary named on its first line owns the
    flags on the rest. Comments are blanked: a comment naming a flag of some
    OTHER tool sits inside these calls, and reading it would be a false alarm.
    String literals are kept -- that is where the flags live.
    """
    src = "\n".join(lines)
    text = list(lines)
    spans, start = [], None
    try:
        for tok in tokenize.generate_tokens(io.StringIO(src).readline):
            if tok.type == tokenize.COMMENT:
                text[tok.start[0] - 1] = text[tok.start[0] - 1][:tok.start[1]]
            elif tok.type == tokenize.NEWLINE:
                if start is not None:
                    spans.append((start, tok.end[0]))
                    start = None
            elif tok.type in (tokenize.NL, tokenize.INDENT, tokenize.DEDENT,
                              tokenize.ENDMARKER):
                continue
            elif start is None:
                start = tok.start[0]
    except (tokenize.TokenError, IndentationError, SyntaxError):
        return                           # unparseable: not ours to judge
    for a, b in spans:
        yield a, [(i, text[i - 1]) for i in range(a, min(b, len(text)) + 1)]


def check_script(path, accepts):
    """Return sorted (line_no, tool, flag) triples the tool would reject."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().split("\n")

    is_py = path.endswith(".py")
    flag_re = PY_FLAG_RE if is_py else FLAG_RE
    blocks = list(python_blocks(lines) if is_py else statement_blocks(lines))

    # Which single binary, if any, does each statement invoke? Two binaries in
    # one statement is ambiguous, so that statement is left unattributed.
    owners = []
    for _, block in blocks:
        text = "\n".join(t for _, t in block)
        tools = set(TOOL_RE.findall(text)) & set(accepts)
        owners.append(tools.pop() if len(tools) == 1 else None)

    # An argument array is judged against the binary whose statement expands it.
    array_tool = {}
    if is_py:
        # Names bound to a list literal, then mentioned in a tool's statement.
        # A list used by two different binaries is dropped rather than guessed
        # at: a false alarm is a gate nobody can get past.
        named = set()
        for _, block in blocks:
            m = PY_ARRAY_RE.match(block[0][1])
            if m:
                named.add(m.group(1))
        seen = {}
        for (_, block), tool in zip(blocks, owners):
            if not tool:
                continue
            text = "\n".join(t for _, t in block)
            for name in named:
                if re.search(r"\b%s\b" % re.escape(name), text):
                    seen.setdefault(name, set()).add(tool)
        array_tool = {n: next(iter(t)) for n, t in seen.items() if len(t) == 1}
    else:
        for (_, block), tool in zip(blocks, owners):
            if tool:
                for name in EXPAND_RE.findall("\n".join(t for _, t in block)):
                    array_tool.setdefault(name, tool)

    problems = set()
    for (_, block), tool in zip(blocks, owners):
        owner = tool
        if owner is None and block:
            m = (PY_ARRAY_RE if is_py else ARRAY_RE).match(block[0][1])
            owner = array_tool.get(m.group(1)) if m else None
        if owner is None:
            continue                     # not attributable -- not ours to judge
        for line_no, text in block:
            for flag in flag_re.findall(text):
                if flag not in accepts[owner]:
                    problems.add((line_no, owner, flag))

    return sorted(problems)


def broken_continuations(path):
    """Return line numbers where a comment interrupts a \\-continued command.

        cmd arg1 \\
            # explanation            <-- swallows the rest of the line
            arg2 \\                  <-- now runs as its own command

    Valid shell, accepted by `bash -n`, and it truncates the command.
    """
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().split("\n")

    sc = _Scanner()
    bad, continued = [], False
    for i, raw in enumerate(lines, 1):
        code, in_heredoc = sc.scan(raw)
        if in_heredoc:
            continue
        if continued and raw.lstrip().startswith("#"):
            bad.append(i)
        continued = code.rstrip().endswith("\\")
    return bad


PRINTCMD_RE = re.compile(r'static void print_cmd\([^)]*\)[^{]*\{(.*?)\n\}', re.S)


def printcmd_gaps(accepts):
    """Flags a parser accepts that its --print_cmd printer never mentions."""
    gaps = {}
    for tool, sources in TOOL_SOURCES.items():
        body = None
        for rel in sources:
            with open(os.path.join(REPO, rel), encoding="utf-8", errors="replace") as fh:
                m = PRINTCMD_RE.search(fh.read())
            if m:
                body = m.group(1)
                break
        if body is None:
            gaps[tool] = ["(no print_cmd() found at all)"]
            continue
        # Flags live INSIDE format strings -- printf(" --out_dir %s", ...) --
        # so match them anywhere in the body, not as standalone literals.
        printed = set(re.findall(r'--[A-Za-z0-9_-]+', body))
        # --help and --version print and exit, so they are never part of a
        # command line that runs anything. Everything else must be covered.
        missing = accepts[tool] - printed - {"--help", "--version"}
        if missing:
            gaps[tool] = sorted(missing)
    return gaps


def main():
    verbose = "--verbose" in sys.argv
    accepts = accepted_flags()
    if verbose:
        for tool in sorted(accepts):
            print("  %-18s %2d flags accepted" % (tool, len(accepts[tool])))

    scripts = []
    for d in SCRIPT_DIRS:
        full = os.path.join(REPO, d)
        if not os.path.isdir(full):
            continue
        # Runners, whatever they are written in. A Python runner passes the same
        # flags to the same binaries and drifts from them the same way.
        exts = (".sh", ".py") if d == "pipeline" else (".sh",)
        scripts += [os.path.join(full, f)
                    for f in sorted(os.listdir(full)) if f.endswith(exts)]

    total = 0
    for tool, missing in sorted(printcmd_gaps(accepts).items()):
        for flag in missing:
            print("%s: --print_cmd never prints %s" % (tool, flag))
            total += 1

    for path in scripts:
        rel = os.path.relpath(path, REPO)
        for line_no, tool, flag in check_script(path, accepts):
            print("%s:%d: %s does not accept %s" % (rel, line_no, tool, flag))
            total += 1
        for line_no in ([] if path.endswith(".py") else broken_continuations(path)):
            print("%s:%d: a comment here truncates the continued command above it"
                  % (rel, line_no))
            total += 1

    if total:
        print("%d problem(s) across %d scripts" % (total, len(scripts)))
        return 1
    print("ok: %d scripts pass only flags their binaries accept, no comment "
          "breaks a continued command, and every --print_cmd covers its parser"
          % len(scripts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
