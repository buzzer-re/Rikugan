# BNIL Reading & Writing Guide

## Reading IL via Rikugan Tools

```
get_il("0x401000", "mlil")
```

Returns indexed IL instructions with addresses. Each instruction has an expression index (used by `il_nop_expr`, `il_replace_expr`) and an address (used by `il_set_condition`).

```
get_il_block("0x401000", 3, "mlil")
```

Returns one block's instructions. Use after `get_il` to drill into specific blocks.

```
get_cfg("0x401000", "mlil")
```

Returns block structure, edges (with types), back edges, dominators, immediate dominators, and loop headers. Essential for understanding CFF dispatchers.

```
track_variable_ssa("0x401000", "var_18", "mlil")
```

Returns every SSA version of the variable: where defined, what value (if constant), where used. Shows the complete data flow for that variable.

## The IL Tower

```
Assembly (architecture-specific)
    ↓  lifting
LLIL — Low Level IL
    ↓  variable recovery, type inference
MLIL — Medium Level IL
    ↓  expression folding, dead code elimination
HLIL — High Level IL (decompiler output)
```

Each level also has an SSA form (LLIL SSA, MLIL SSA) where every variable is assigned exactly once. SSA form is for **analysis only** — never modify it directly.

### LLIL

- ~28 operations covering all architectural concepts.
- Registers, flags, memory loads/stores explicitly visible.
- 1:1 mapping to machine instructions.
- Useful for: instruction-level pattern matching, finding junk bytes, anti-disassembly patterns.
- Operations: `LLIL_SET_REG`, `LLIL_REG`, `LLIL_LOAD`, `LLIL_STORE`, `LLIL_PUSH`, `LLIL_POP`, `LLIL_IF`, `LLIL_GOTO`, `LLIL_CALL`, `LLIL_RET`, `LLIL_ADD`, `LLIL_SUB`, `LLIL_XOR`, `LLIL_AND`, `LLIL_OR`, `LLIL_CMP_*`, `LLIL_NOP`.

### MLIL

- Registers and stack abstracted into typed variables.
- No `PUSH`/`POP` — stack is gone.
- `LLIL_SET_REG` / `LLIL_REG` become `MLIL_SET_VAR` / `MLIL_VAR`.
- Constants propagated, data flow computed.
- **Best level for deobfuscation** — architecture-agnostic, good pattern matching, SSA available.
- Operations: `MLIL_SET_VAR`, `MLIL_VAR`, `MLIL_CONST`, `MLIL_IF`, `MLIL_GOTO`, `MLIL_CALL`, `MLIL_RET`, `MLIL_ADD`, `MLIL_SUB`, `MLIL_XOR`, `MLIL_AND`, `MLIL_OR`, `MLIL_MUL`, `MLIL_DIVU`, `MLIL_CMP_*`, `MLIL_NOP`.

### HLIL

- C-like pseudocode with expression folding.
- Deep nesting — expressions are trees, not flat sequences.
- Dead code eliminated, variables simplified.
- Useful for: verification after deobfuscation, reading final output.
- **Not ideal for pattern matching** — folding makes patterns harder to detect.

## Modification Model

Binary Ninja IL modification works differently from IDA microcode:

### Built-in Tools (Path A)

Built-in tools handle all the complexity for you:

- `il_set_condition(addr, cond_addr, "true")` → finds the IF instruction, replaces condition with constant, finalizes, updates analysis.
- `il_nop_expr(addr, expr_index)` → replaces expression with NOP, finalizes, updates.
- `il_replace_expr(addr, expr_index, "const", value)` → replaces expression with constant, finalizes, updates.

**You don't need to call finalize() or generate_ssa_form() when using built-in tools.**

### execute_python Modifications (Path C)

When using `execute_python` for IL modification, you must handle the lifecycle yourself:

```python
# Get the function and IL
func = bv.get_function_at(addr)
il_func = func.mlil  # or func.llil

# Find and replace an expression
for instr in il_func.instructions:
    if should_replace(instr):
        new_expr = il_func.const(instr.size, simplified_value)
        il_func.replace_expr(instr, new_expr)

# MUST call both after modifications
il_func.finalize()           # reconstructs basic blocks
il_func.generate_ssa_form()  # rebuilds SSA and data flow
```

**Key constraints:**
- `replace_expr` works on non-SSA IL only (LLIL or MLIL, not their SSA forms).
- After replacement, old expressions become "dangling" (no longer referenced) — this is fine.
- `finalize()` does a BFS from instruction 0 to reconstruct basic blocks.
- `generate_ssa_form()` rebuilds SSA form and data flow calculations.
- Modifications at LLIL propagate up to MLIL and HLIL.
- Modifications at MLIL propagate up to HLIL but NOT down to LLIL.

### Workflow Modifications (Path B)

Inside a workflow activity, use `analysis_context` to access IL:

```python
def transform(analysis_context, il_func):
    # il_func is already the correct IL level
    for instr in il_func.instructions:
        if should_modify(instr):
            new = il_func.const(instr.size, value)
            il_func.replace_expr(instr, new)
    il_func.finalize()
    il_func.generate_ssa_form()
```

## Expression Types (MLIL)

Expressions have an `operation` attribute. Check `operation.name` for the type:

| Operation name | Meaning | Key attributes |
|---|---|---|
| `MLIL_SET_VAR` | Variable assignment | `.dest` (var), `.src` (expression) |
| `MLIL_VAR` | Variable read | `.src` (var) |
| `MLIL_CONST` | Constant value | `.constant` (int value) |
| `MLIL_IF` | Conditional branch | `.condition` (expression), `.true`/`.false` (targets) |
| `MLIL_GOTO` | Unconditional jump | `.dest` (target) |
| `MLIL_CALL` | Function call | `.dest` (expression), `.params` (list) |
| `MLIL_RET` | Return | `.src` (list of return values) |
| `MLIL_ADD` | Addition | `.left`, `.right` |
| `MLIL_SUB` | Subtraction | `.left`, `.right` |
| `MLIL_XOR` | Bitwise XOR | `.left`, `.right` |
| `MLIL_AND` | Bitwise AND | `.left`, `.right` |
| `MLIL_OR` | Bitwise OR | `.left`, `.right` |
| `MLIL_MUL` | Multiplication | `.left`, `.right` |
| `MLIL_SX` | Sign extend | `.src` |
| `MLIL_ZX` | Zero extend | `.src` |
| `MLIL_CMP_E` | Compare equal | `.left`, `.right` |
| `MLIL_CMP_NE` | Compare not equal | `.left`, `.right` |
| `MLIL_NOP` | No operation | — |

## SSA Form for Analysis

SSA is critical for tracking state variables in CFF:

```python
# In execute_python:
func = bv.get_function_at(addr)
mlil_ssa = func.mlil.ssa_form

# Find all definitions of a variable
for ssa_var in mlil_ssa.ssa_vars:
    if ssa_var.var.name == "state_var":
        defn = mlil_ssa.get_ssa_var_definition(ssa_var)
        uses = mlil_ssa.get_ssa_var_uses(ssa_var)
        print(f"v{ssa_var.version}: defined at {defn}, {len(uses)} uses")
```

**Or use the built-in tool:** `track_variable_ssa("0x401000", "state_var", "mlil")` — does the same thing without execute_python.

## Byte-Level Operations

When IL modification isn't enough:

```python
# Via built-in tool:
nop_instructions("0x401000", "0x401004,0x401008,0x40100c")

# Via execute_python:
bv.convert_to_nop(addr)             # NOP one instruction (arch-aware)
bv.write(addr, b"\x90" * length)    # raw byte write
bv.update_analysis_and_wait()       # re-analyze after patching
```

## Key Differences from IDA Microcode

| Aspect | Binary Ninja | IDA Microcode |
|---|---|---|
| IL levels | 3 (LLIL, MLIL, HLIL) + SSA forms | 1 (microcode with 8 maturity levels) |
| Modification target | Non-SSA LLIL or MLIL | `minsn_t` in optimizer callbacks |
| Batch transform | Workflow API (`install_il_workflow`) | `install_microcode_optimizer` |
| After modification | `finalize()` + `generate_ssa_form()` | `blk.mark_lists_dirty()` + `mba.mark_chains_dirty()` |
| Architecture | Agnostic at MLIL+ | Agnostic (microcode is arch-independent) |
| Overlapping code | Handles well (unique strength) | Limited |
| Maturity guard | Not needed (no maturity levels) | MUST check `mba.maturity >= 6` |
| Expression creation | `il_func.const()`, `il_func.nop()` | `make_number()`, direct opcode assignment |
