# Rikugan script guard, run once in the host's Python interpreter.
#
# Defines __rk_exec(src, ns) -> str, a JSON object {"output": str, "error": str | None}:
# - the code is rejected by the AST checks: output is "" and error the violation ("Blocked — ...");
# - otherwise error is None and output is what the script printed: "stdout:\n..." and/or
#   "stderr:\n..." joined by "\n", or "(no output)"; an exception is written to stderr as "Type: msg".
#
# Every name defined here starts with __rk_ (no module docstring either): this runs in the host's shared namespace.

import ast as __rk_ast
import builtins as __rk_builtins
import contextlib as __rk_contextlib
import io as __rk_io
import json as __rk_json

# Modules that must never be imported (directly or via __import__).
__rk_BLOCKED_MODULES = frozenset({"subprocess", "shlex", "pty", "commands"})

# Built-in calls that must never appear.
__rk_BLOCKED_CALLS = frozenset({"exec", "eval", "compile", "__import__"})

# Attribute calls that must never appear (module.func patterns).
__rk_BLOCKED_ATTRS = frozenset(
    {
        ("os", "system"),
        ("os", "popen"),
        ("os", "execl"),
        ("os", "execle"),
        ("os", "execlp"),
        ("os", "execlpe"),
        ("os", "execv"),
        ("os", "execve"),
        ("os", "execvp"),
        ("os", "execvpe"),
        ("os", "spawnl"),
        ("os", "spawnle"),
        ("os", "spawnlp"),
        ("os", "spawnlpe"),
        ("os", "spawnv"),
        ("os", "spawnve"),
        ("os", "spawnvp"),
        ("os", "spawnvpe"),
    }
)

# Builtins removed from the execution namespace to prevent reflective bypasses
# (e.g. __builtins__['__import__'], eval("os.system")).
__rk_REMOVED_BUILTINS = frozenset({"__import__", "exec", "eval", "compile", "breakpoint", "exit", "quit"})


def __rk_safe_builtins():
    """A restricted __builtins__ dict with dangerous names removed."""
    return {k: v for k, v in vars(__rk_builtins).items() if k not in __rk_REMOVED_BUILTINS}


def __rk_check_ast(code):
    """The violation message for blocked constructs in `code`, or None when it is safe."""
    try:
        tree = __rk_ast.parse(code)
    except SyntaxError:
        return "Blocked — code contains a syntax error and cannot be validated"

    for node in __rk_ast.walk(tree):
        # import subprocess / from subprocess import ...
        if isinstance(node, __rk_ast.Import):
            for alias in node.names:
                root = alias.name.split(".")[0]
                if root in __rk_BLOCKED_MODULES:
                    return f"Blocked — import of disallowed module '{alias.name}'"

        elif isinstance(node, __rk_ast.ImportFrom):
            if node.module:
                root = node.module.split(".")[0]
                if root in __rk_BLOCKED_MODULES:
                    return f"Blocked — import from disallowed module '{node.module}'"

        # exec(), eval(), compile(), __import__()
        elif isinstance(node, __rk_ast.Call):
            func = node.func
            if isinstance(func, __rk_ast.Name) and func.id in __rk_BLOCKED_CALLS:
                return f"Blocked — call to disallowed built-in '{func.id}()'"

            # os.system(), os.popen(), os.exec*(), os.spawn*()
            if isinstance(func, __rk_ast.Attribute) and isinstance(func.value, __rk_ast.Name):
                pair = (func.value.id, func.attr)
                if pair in __rk_BLOCKED_ATTRS:
                    return f"Blocked — call to disallowed '{pair[0]}.{pair[1]}()'"
                if func.value.id == "os" and (func.attr.startswith("exec") or func.attr.startswith("spawn")):
                    return f"Blocked — call to disallowed 'os.{func.attr}()'"

        # subscript access to __builtins__ (e.g. __builtins__['__import__'])
        elif isinstance(node, __rk_ast.Subscript):
            if isinstance(node.value, __rk_ast.Name) and node.value.id == "__builtins__":
                return "Blocked — direct subscript access to __builtins__"

    return None


def __rk_exec(src, ns):
    """Check `src`, run it in `ns` with restricted builtins, return the JSON result described above."""
    violation = __rk_check_ast(src)
    if violation:
        return __rk_json.dumps({"output": "", "error": violation})

    # restrict __builtins__ even when the namespace was handed full access
    ns_builtins = ns.get("__builtins__")
    if ns_builtins is None or ns_builtins is __rk_builtins or ns_builtins is vars(__rk_builtins):
        ns["__builtins__"] = __rk_safe_builtins()
    elif isinstance(ns_builtins, dict):
        for name in __rk_REMOVED_BUILTINS:
            ns_builtins.pop(name, None)

    stdout_buf = __rk_io.StringIO()
    stderr_buf = __rk_io.StringIO()
    with __rk_contextlib.redirect_stdout(stdout_buf), __rk_contextlib.redirect_stderr(stderr_buf):
        try:
            exec(src, ns)
        except Exception as e:
            stderr_buf.write(f"{type(e).__name__}: {e}\n")

    stdout = stdout_buf.getvalue()
    stderr = stderr_buf.getvalue()
    parts = []
    if stdout:
        parts.append(f"stdout:\n{stdout}")
    if stderr:
        parts.append(f"stderr:\n{stderr}")
    if not parts:
        parts.append("(no output)")
    return __rk_json.dumps({"output": "\n".join(parts), "error": None})
