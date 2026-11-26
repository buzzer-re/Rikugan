# Algorithm Reference

Methodology and recognition patterns for each deobfuscation technique in Binary Ninja. This reference teaches **how to identify and adapt** — every binary is different.

**Workflow for every technique:** Read MLIL → Identify the specific variant → Design a targeted fix → Apply with built-in tools or execute_python → Redecompile → Verify → Iterate.

---

## CFF — Control Flow Flattening Removal

### Recognition

Use `get_cfg(addr, "mlil")` and `get_il(addr, "mlil")`. CFF has these signatures:

- **Back edges in CFG**: `get_cfg` shows back edges and loop headers. The dispatcher is a loop that everything flows through.
- **State variable**: One variable compared against many constants across the function. Use `track_variable_ssa` on suspected variables — the one with the most constant definitions and comparison uses is the state variable.
- **Dispatcher blocks**: Blocks containing `MLIL_IF` instructions that compare the state variable against constants. There may be multiple dispatchers (chained comparisons).
- **Handler blocks**: Blocks that do actual work (real logic), then assign a new constant to the state variable and jump back to a dispatcher.

BN's advantage: MLIL abstracts stack variables into named vars. The state variable appears as a clean `var_18 = 0x1234` rather than raw stack stores. `track_variable_ssa` shows every assignment and use directly.

### Approach

1. **Find the state variable.** Read MLIL. Look for a variable that:
   - Is assigned constant values in many different blocks
   - Is compared in conditional branches (`MLIL_IF` / `MLIL_CMP_E`)
   - Has SSA versions spanning most of the function
   Use `track_variable_ssa` to confirm — the variable with the most constant-definition SSA versions is the state variable. **No entropy filtering** — state values may be sequential or small.

2. **Map the CFG.** Use `get_cfg` to identify:
   - Loop headers (dispatcher blocks)
   - Back edges (handler → dispatcher transitions)
   - Dominators (helps distinguish dispatchers from handlers)

3. **Map states to handlers.** For each `MLIL_IF` in a dispatcher block that compares the state variable to a constant, the taken branch target is the handler for that state value.

4. **Determine handler successors.** For each handler block: use `track_variable_ssa` or read the MLIL to find what constant the handler assigns to the state variable. That constant maps to the next handler via the dispatcher mapping.

5. **Rewire or force branches.** Two approaches:
   - **Per-branch forcing**: For each dispatcher comparison, if you know which handler should execute next, use `il_set_condition` to force the correct branch. This is simpler but requires many individual operations.
   - **Full CFG rewrite**: Use `execute_python` with `replace_expr` to rewrite handler tail jumps (change goto targets from dispatcher to next handler directly). Then NOP state variable assignments.

6. **Clean up.** NOP state variable assignments with `il_nop_expr`. Remove dead dispatcher blocks with `il_remove_block` if BN doesn't prune them automatically. Redecompile.

### Key decisions per binary

- **MLIL vs LLIL?** Start with MLIL — it's cleaner and architecture-agnostic. Fall back to LLIL only if the state variable isn't visible at MLIL (rare, but possible with very early optimization).
- **Per-branch vs full rewrite?** For small functions (<20 handlers), per-branch forcing with `il_set_condition` is simpler. For large functions, `execute_python` with programmatic rewriting is more practical.
- **Dominator-based approach?** BN's CFG includes dominator info. Dispatcher blocks dominate all handler blocks. Handlers that dominate nothing (leaf handlers) are easier to start with.

---

## Opaque Predicates

### Recognition

- **Constant conditions in MLIL**: An `MLIL_IF` where the condition operand is `MLIL_CONST` (BN may have already constant-propagated it). Read MLIL — if you see `if (1)` or `if (0)`, it's already resolved.
- **Algebraically constant**: Expressions like `(x * (x - 1)) & 1 == 0` that are always true. These survive constant propagation because the operands aren't constants.
- **Sub-and-jump / XOR-and-jump patterns**: Common OLLVM patterns visible at LLIL — `sub reg, reg; jne` (always falls through because result is always 0).
- **Same predicate everywhere**: If many functions have the same conditional pattern, it's likely an obfuscator template.

### Approach

**For already-resolved predicates** (condition is a constant in MLIL):
- Use `il_set_condition(addr, cond_addr, "true")` or `"false"`.
- Dead branch becomes unreachable; BN prunes on reanalysis.

**For algebraically constant predicates**:
- Use `track_variable_ssa` to trace the condition's operands. If all operands trace back to constants, compute the result.
- For non-trivial identities, use `execute_python` with Z3:
  ```python
  import z3
  x = z3.BitVec('x', 32)
  solver = z3.Solver()
  solver.add(z3.Not((x * (x - 1)) & 1 == 0))
  if solver.check() == z3.unsat:
      print("Always true — force taken")
  ```
- Once proven, use `il_set_condition` to force the branch.

**For batch removal** (same pattern in many functions):
- Use `install_il_workflow` with a transform that detects the pattern and replaces it.

---

## MBA — Mixed Boolean-Arithmetic

### Recognition

Complex boolean-arithmetic expressions that compute simple results. In MLIL, look for chains of `MLIL_XOR`, `MLIL_AND`, `MLIL_OR`, `MLIL_ADD` operating on the same variables.

### Approach

**When both operands are constants** (BN may not have folded them):
- Read the expression index from `get_il`.
- Compute the simplified value.
- Use `il_replace_expr(addr, expr_index, "const", simplified_value, size)`.

**When operands are symbolic** — pattern-match known identities:

| Obfuscated form | Simplified |
|---|---|
| `(x ^ y) + 2*(x & y)` | `x + y` |
| `(x \| y) + (x & y)` | `x + y` |
| `(x \| y) - (x & ~y)` | `y` |
| `~(~x & ~y)` | `x \| y` |
| `~(~x \| ~y)` | `x & y` |
| `(x & M) \| (x & ~M)` | `x` (identity mask) |

**Key decisions:**
- Check HLIL first — BN's decompiler sometimes simplifies MBA automatically.
- Work bottom-up: simplify inner expressions first (they may feed into outer ones).
- For identities not in the table, use Z3 equivalence checking via `execute_python`.
- Multiple passes needed — MBA can be layered.

---

## Instruction Substitution Reversal

### Recognition

Simple operations replaced with equivalent complex sequences. Common in OLLVM. At MLIL level, these appear as multi-expression computations that should be a single operation.

### Common substitution patterns (match and reverse)

| Original | Substituted forms |
|---|---|
| `a + b` | `a - (-b)`, `(a ^ b) + 2*(a & b)`, `(a \| b) + (a & b)` |
| `a - b` | `a + (-b)`, `(a ^ b) - 2*(~a & b)` |
| `a ^ b` | `(a \| b) - (a & b)`, `(~a & b) \| (a & ~b)` |
| `a & b` | `(a \| b) - (a ^ b)`, `~(~a \| ~b)` |
| `a \| b` | `(a & b) + (a ^ b)`, `~(~a & ~b)` |

### Approach

- Read MLIL to identify the specific substitution pattern used in this binary.
- For constant-result substitutions: compute the value, use `il_replace_expr` with `const`.
- For symbolic substitutions with execute_python: iterate MLIL instructions, pattern-match the complex form, replace with the simplified expression.
- Don't enumerate every pattern. Focus on what you see in the specific binary.

---

## Dead Code / Junk Instructions

### Recognition

- **Dead stores**: A variable is assigned but never read before being overwritten. Use `track_variable_ssa` — if a version has 0 uses, its definition is dead.
- **Self-operations**: `x ^ x` (always 0), `x - x` (always 0), `x | 0`, `x & 0xFF..FF`, `x * 1`, `x + 0`.
- **Post-CFF cleanup**: After CFF removal, state variable assignments are all dead.

### Approach

- Use `track_variable_ssa` to find dead definitions (SSA versions with no uses).
- NOP dead definitions with `il_nop_expr`.
- For self-operations visible in MLIL: replace with constant (`il_replace_expr`) or NOP.

---

## Bogus Control Flow (BCF)

BCF is opaque predicates + junk blocks. The removal strategy is the same as opaque predicate elimination. Once the predicate is forced, the junk block becomes unreachable. Use `il_remove_block` if BN doesn't prune it automatically on reanalysis.

---

## Anti-Disassembly

Byte-level problem, byte-level fix.

**BN's advantage**: BN handles overlapping code better than most tools — check the disassembly view first. BN may already have resolved the issue.

**Common patterns:**
- Junk bytes after unconditional jumps (e.g., `jmp +2; db 0xE8`).
- Overlapping instructions exploiting variable-length encoding.

**Fix:**
- `nop_instructions(func_addr, "0x401004,0x401006")` to NOP junk byte addresses.
- `execute_python` with `bv.add_user_function(addr)` to redefine function boundaries.
- `write_bytes(addr, "90")` for single-byte fixes.

---

## VM Boundary Detection

Not reversible with IL-level tools. Document and move on.

1. **Identify**: VM entry point, bytecode buffer, handler table, dispatch mechanism.
2. **Document**: Use `set_comment` and `rename_address` to annotate VM-related addresses.
3. **Scope**: Focus deobfuscation on non-virtualized code paths.
4. **Advanced**: BN supports custom architecture plugins — a VM's ISA can be lifted to BNIL for analysis (but this is a major undertaking). Alternatively, use Triton/Miasm via `execute_python` for symbolic execution of VM handlers.

---

## Combining Techniques

1. **Read first** — get MLIL and CFG to understand what's present.
2. **Check HLIL** — BN may have already simplified some layers.
3. **Prioritize** — CFF is usually the outermost layer. Remove it first.
4. **Work incrementally** — one technique at a time, redecompile after each.
5. **Verify** — if output doesn't improve, the pattern match was wrong. Read again.
6. **Layer order**: typically CFF → opaque predicates/BCF → MBA → instruction substitution → dead code cleanup.
7. **Batch when possible** — if the same pattern exists across many functions, `install_il_workflow` saves time.
