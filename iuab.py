#!/usr/bin/env python3
"""iuab: interpreter for the "I use arch btw" programming language."""
import ast
import operator
import sys

BANNER = "i use arch btw"

BINOPS = {
    ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul,
    ast.Div: operator.truediv, ast.FloorDiv: operator.floordiv,
    ast.Mod: operator.mod, ast.Pow: operator.pow,
}
CMPOPS = {
    ast.Eq: operator.eq, ast.NotEq: operator.ne, ast.Lt: operator.lt,
    ast.LtE: operator.le, ast.Gt: operator.gt, ast.GtE: operator.ge,
}


class ArchError(Exception):
    pass


def evaluate(src, env):
    def ev(n):
        if isinstance(n, ast.Expression):
            return ev(n.body)
        if isinstance(n, ast.Constant) and isinstance(n.value, (int, float, str, bool)):
            return n.value
        if isinstance(n, ast.Name):
            if n.id not in env:
                raise ArchError(f"package '{n.id}' was not found (did you pacman -S it?)")
            return env[n.id]
        if isinstance(n, ast.BinOp) and type(n.op) in BINOPS:
            return BINOPS[type(n.op)](ev(n.left), ev(n.right))
        if isinstance(n, ast.UnaryOp) and isinstance(n.op, (ast.USub, ast.Not)):
            v = ev(n.operand)
            return -v if isinstance(n.op, ast.USub) else (not v)
        if isinstance(n, ast.BoolOp):
            vals = [ev(v) for v in n.values]
            return all(vals) if isinstance(n.op, ast.And) else any(vals)
        if isinstance(n, ast.Compare):
            left = ev(n.left)
            for op, comp in zip(n.ops, n.comparators):
                if type(op) not in CMPOPS:
                    raise ArchError("unsupported comparison")
                right = ev(comp)
                if not CMPOPS[type(op)](left, right):
                    return False
                left = right
            return True
        raise ArchError("unsupported expression")
    try:
        return ev(ast.parse(src.strip(), mode="eval"))
    except SyntaxError:
        raise ArchError(f"syntax error in expression: {src.strip()!r}")


def parse_block(lines, i, enders):
    """Parse statements until a line in `enders`; returns (stmts, index, ender)."""
    stmts = []
    while i < len(lines):
        no, text = lines[i]
        word = text.split(None, 1)[0]
        if text in enders:
            return stmts, i, text
        if text.startswith("while "):
            body, i, _ = parse_block(lines, i + 1, {"done"})
            stmts.append(("while", no, text[6:], body))
        elif text.startswith("if "):
            body, i, end = parse_block(lines, i + 1, {"fi", "else"})
            other = []
            if end == "else":
                other, i, _ = parse_block(lines, i + 1, {"fi"})
            stmts.append(("if", no, text[3:], body, other))
        elif text.startswith("pacman -S "):
            parts = text[10:].split(None, 1)
            if len(parts) != 2:
                raise ArchError(f"line {no}: usage: pacman -S <name> <expr>")
            stmts.append(("set", no, parts[0], parts[1]))
        elif word == "echo":
            stmts.append(("echo", no, text[4:]))
        else:
            raise ArchError(f"line {no}: command not found: {word}")
        i += 1
    if enders:
        raise ArchError(f"missing {' or '.join(sorted(enders))}")
    return stmts, i, None


def run(stmts, env, out):
    for s in stmts:
        kind = s[0]
        try:
            if kind == "set":
                env[s[2]] = evaluate(s[3], env)
            elif kind == "echo":
                v = evaluate(s[2], env)
                out(str(v).lower() if isinstance(v, bool) else str(v))
            elif kind == "while":
                while evaluate(s[2], env):
                    run(s[3], env, out)
            elif kind == "if":
                run(s[3] if evaluate(s[2], env) else s[4], env, out)
        except ArchError as e:
            if str(e).startswith("line "):
                raise
            raise ArchError(f"line {s[1]}: {e}")


def interpret(source, out=print):
    lines = []
    for no, raw in enumerate(source.splitlines(), 1):
        text = raw.split("#", 1)[0].strip()
        if text:
            lines.append((no, text))
    if not lines or lines[0][1].lower() != BANNER:
        raise ArchError("not Arch. First line must be: I use arch btw")
    stmts, _, _ = parse_block(lines[1:], 0, set())
    run(stmts, {}, out)


def main(argv):
    if len(argv) != 2:
        print("usage: iuab.py <file.arch>", file=sys.stderr)
        return 2
    with open(argv[1]) as f:
        src = f.read()
    try:
        interpret(src)
    except ArchError as e:
        print(f"iuab: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
