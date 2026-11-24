---
name: ctf
description: Capture-the-flag reverse engineering — find the flag efficiently
---
Task: CTF Challenge. You are solving a capture-the-flag reverse engineering challenge. The goal is finding the flag.

## Approach

Be targeted and efficient. CTF binaries are usually small, purpose-built, and contain a clear solve path. Don't over-analyze — find the check/validation function, understand the constraint, solve it.

## Workflow

1. `get_binary_info` + `list_functions` — orient yourself, find main or entry (batch these)
2. `decompile_function` on main — identify the input path and validation logic
3. Trace the check function: usually a comparison, hash check, or transformation chain
4. Identify the algorithm: XOR, custom cipher, hash, math constraints, maze/game, VM-based
5. `search_strings` for flag format strings (CTF{, flag{, HTB{, etc.)
6. Solve: extract the key/flag directly, reverse the transformation, or write a solver

## Common Patterns

- **Flag format strings** visible in `list_strings` or `search_strings`
- **Input validation** concentrated in a single function
- **XOR with static key** — extract key and data, XOR to get flag
- **Base64 or custom encoding** — identify the table, decode
- **Constraint satisfaction** — extract constraints, use z3 via `execute_python`
- **Anti-debug checks** (ptrace, IsDebuggerPresent) guarding the real logic — bypass or ignore
- **Multi-stage**: unpacking → decryption → flag check
- **VM-based**: custom bytecode interpreter — map opcodes, trace execution, extract constraints

## Solving Strategies

**Direct extraction:** If the flag is compared byte-by-byte or XOR'd with
a known key, extract both operands and compute the flag directly.

**Constraint solving:** For complex validation (many conditions, polynomial
checks, matrix transforms), extract constraints and write a z3 solver:
```python
from z3 import *
s = Solver()
flag = [BitVec(f'c{i}', 8) for i in range(N)]
# Add constraints from decompiled validation...
s.add(...)
if s.check() == sat:
    m = s.model()
    print(''.join(chr(m[c].as_long()) for c in flag))
```

**Transformation reversal:** If the input goes through a series of
reversible transforms (XOR, rotate, shuffle, substitution), reverse
each step in order.

## Tips

- If you find encrypted/encoded data, try to reverse the algorithm from the decompiled code
- For constraint solving, write and execute a Python script with `execute_python`
- Focus on the solve path — don't enumerate every function or produce threat reports
- Check `xrefs_to` on comparison/validation functions to find where the flag is checked
- Look at string xrefs — flag-related strings often lead directly to the validation logic
- If stuck, check for hardcoded keys or constants near the comparison code
