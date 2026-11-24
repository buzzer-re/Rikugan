# Binary Ninja Deobfuscation Guide

> **Environment:** Binary Ninja 4.x+ with decompiler | `binaryninja` Python module available via `execute_python`

Full tool listing in `tools.md`. Recognition patterns and methodology in `algorithm-reference.md`. IL reading/writing reference in `il-guide.md`.

## Two Deobfuscation Paths

### Path A: Built-in IL Tools (preferred)

Use Rikugan's built-in tools for targeted, per-function deobfuscation. Read the IL, identify patterns, apply precise modifications.

```
1. get_il(addr, "mlil")                   — read IL, identify obfuscation patterns
2. get_cfg(addr, "mlil")                  — understand CFG structure (dispatchers, loops)
3. track_variable_ssa(addr, var, "mlil")  — trace state variable through def-use chains
4. il_set_condition / il_nop_expr / il_replace_expr — apply targeted fixes
5. redecompile_function(addr)             — verify improvement
6. (iterate: read → fix more → redecompile)
```

### Path B: Workflow Transform (via execute_python)

Use `install_il_workflow` when a pattern repeats across many functions and you need batch processing. The workflow hooks into BN's analysis pipeline.

```python
def transform(analysis_context, il_func):
    # il_func is the LLIL or MLIL function
    # Iterate instructions, match patterns, replace
    for i, instr in enumerate(il_func.instructions):
        if should_simplify(instr):
            new_expr = il_func.const(instr.size, simplified_value)
            il_func.replace_expr(instr, new_expr)
    il_func.finalize()
    il_func.generate_ssa_form()
```

### Path C: execute_python (complex cases)

Use for Z3 solving, emulation-based decryption, multi-function analysis, or anything requiring full `binaryninja` API access beyond what built-in tools provide.

### When to Use Which

| Scenario | Path |
|---|---|
| Targeted per-function deobfuscation | A — built-in tools |
| Force specific opaque predicates | A — `il_set_condition` |
| NOP junk expressions by index | A — `il_nop_expr` |
| Replace MBA with simplified constant | A — `il_replace_expr` |
| Same pattern across dozens of functions | B — `install_il_workflow` |
| CFF unflattening (complex CFG rewrite) | C — `execute_python` with full BN API |
| Z3 symbolic solving | C — `execute_python` |
| String decryption with emulation | C — `execute_python` |

## Technique Rules

### CFF (Control Flow Flattening)

- Use `get_cfg` at MLIL to identify the dispatcher loop (back edges, loop headers).
- Use `track_variable_ssa` to find the state variable — the variable with the most constant comparisons.
- **No entropy filtering** — state values may not look random.
- Assume **multiple dispatcher blocks** — any block comparing the state var is a dispatcher.
- BN's MLIL abstracts stack variables into named vars — easier to track than raw registers.
- After identifying handlers and their successors, use `il_set_condition` to force dispatcher branches or `execute_python` for full CFG rewriting.
- BN's dominator tree (`get_cfg` output) helps identify which blocks are handlers vs dispatchers.

### Opaque Predicates

- Read MLIL — BN's constant propagation may already resolve some predicates.
- If a conditional branch has a constant condition in MLIL, use `il_set_condition` to force it.
- For algebraically constant predicates, use `execute_python` with Z3 to prove, then force with `il_set_condition`.
- BN's `track_variable_ssa` can trace the predicate's operands to their definitions — if all defs are constants, the predicate is constant.
- After forcing, BN automatically prunes dead code on reanalysis.

### MBA (Mixed Boolean-Arithmetic)

- Read MLIL to identify complex expressions that should be simpler.
- When both operands are constants in MLIL: compute the result, use `il_replace_expr` with `replacement_type='const'`.
- For symbolic MBA: use `execute_python` with pattern matching or Z3 equivalence checking.
- BN's HLIL sometimes simplifies MBA automatically — check HLIL before manual work.

### Bogus Control Flow (BCF)

- Same as opaque predicate removal — force the predicate, dead block becomes unreachable.
- Use `il_remove_block` to explicitly remove junk blocks if needed (BN may not prune them automatically in all cases).

### Instruction Substitution

- Read MLIL to spot unnecessary complexity (e.g., `x - (-y)` instead of `x + y`).
- Use `il_replace_expr` with `replacement_type='const'` when the simplified value is a constant.
- For symbolic substitutions, use `execute_python` to pattern-match and rewrite.

### Dead Code / Junk

- After CFF removal, state variable assignments become dead — NOP them with `il_nop_expr`.
- Use `track_variable_ssa` to confirm a variable has no uses after a definition — safe to NOP.
- Self-XOR, add-zero, multiply-by-one patterns visible in MLIL.

### Anti-Disassembly

- BN handles overlapping code better than most disassemblers — check if BN already resolved it.
- For junk bytes: use `nop_instructions` to NOP at the byte level.
- For function boundary issues: use `execute_python` to call `bv.add_user_function()` or redefine boundaries.

## Critical Rules

| Rule | Detail |
|---|---|
| **MLIL for deobfuscation** | Always prefer MLIL over LLIL/HLIL for pattern matching and analysis |
| **SSA for analysis, non-SSA for modification** | Use SSA form (`track_variable_ssa`) to analyze data flow. Modify the non-SSA IL (`il_replace_expr`, `il_nop_expr`). Never modify SSA forms directly — they're regenerated |
| **finalize + generate_ssa_form** | After `replace_expr` in execute_python, always call `il_func.finalize()` then `il_func.generate_ssa_form()`. Built-in tools handle this automatically |
| **Redecompile after every change** | Call `redecompile_function` after modifications to verify improvement and catch regressions |
| **Expression index, not address** | `il_nop_expr` and `il_replace_expr` take expression indices (from `get_il` output), not addresses. `il_set_condition` takes the branch address |
| **Architecture agnostic** | MLIL is architecture-independent — the same deobfuscation logic works on x86, ARM, MIPS |
| **Workflow for batch** | If the same pattern exists in >5 functions, use `install_il_workflow` instead of per-function fixes |
| **Check HLIL first** | BN may have already simplified the expression at HLIL level. Don't redo work the decompiler already did |
| **Iterative approach** | Complex obfuscation needs multiple passes — fix one layer, redecompile, fix the next |

## String Decryption

```
1. search_strings / list_strings → very few readable strings → encrypted
2. search_functions for small frequently-called functions → decode stub candidates
3. xrefs_to(decode_func) → all call sites
4. decompile_function(caller) → trace arguments to find encrypted data + key
5. execute_python → reimplement decode logic, compute plaintext
   (bv has full read access: bv.read(addr, length) for encrypted buffers)
6. set_comment at each call site → "decrypted: <plaintext>"
7. rename_function(decode_func, "decrypt_string")
```

## Troubleshooting

**il_set_condition reports "No conditional branch found":**
- The instruction at that address may not be an IF at the IL level. Check `get_il` output — the conditional may be at a different address than expected (MLIL can merge/split instructions).
- Try at LLIL level instead: `il_set_condition(addr, cond_addr, "true", level="llil")`.

**il_replace_expr / il_nop_expr fails:**
- Expression index may be wrong. Re-read `get_il` output to get the correct index.
- The expression may have been modified by a previous operation — reread after each change.

**Decompiled output unchanged after modification:**
- Call `redecompile_function` — BN caches decompilation results.
- IL modifications at LLIL affect MLIL and HLIL. MLIL modifications affect HLIL. But HLIL modifications don't affect lower levels.

**install_il_workflow reports "already exists":**
- Workflows persist for the session. Each name must be unique. Use a different name or restart the session.

**CFF not detected in get_cfg:**
- Try at LLIL level — some CFF patterns are clearer before MLIL lifting.
- State variable may be a stack variable at LLIL that becomes a named var at MLIL. Check both.

**Patch_branch fails on non-x86:**
- `patch_branch` is x86/x64 only. Use `il_set_condition` (architecture-agnostic) or `write_bytes` with architecture-specific NOP encoding.

**BN already simplified the expression:**
- Check HLIL before writing deobfuscation logic. BN's constant propagation and dead code elimination may have already done the work.
