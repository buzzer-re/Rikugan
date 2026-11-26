# IDA Pro Deobfuscation Guide

> **Environment:** IDA Pro 9.1+ with Hex-Rays | `ida-domain>=0.1.0` available | All `ida_*` modules available via `execute_python`

Full tool listing in `tools.md`. Recognition patterns and methodology in `algorithm-reference.md`. Microcode reading/writing reference in `microcode-guide.md`.

## Two Deobfuscation Paths

### Path A: Built-in Microcode Optimizers (preferred)

Use `install_microcode_optimizer` to register Python callbacks that run during every decompilation. This is the primary deobfuscation mechanism — it integrates with Hex-Rays' optimization pipeline.

```
1. get_microcode(addr, "MMAT_LOCOPT")     — read microcode, identify patterns
2. install_microcode_optimizer(name, desc, type, code) — install pattern rule
3. redecompile_function(addr)              — see cleaned output
4. (iterate: read → install more → redecompile)
5. remove_microcode_optimizer(name)        — remove when done
```

**Instruction optimizer** (`optinsn_t`): called per-instruction at MMAT_GENERATED through MMAT_GLBOPT2. Return count of changes.
```python
def optimize(blk, ins):
    # Pattern match on ins.opcode, ins.l, ins.r, ins.d
    # Modify in-place: ins.opcode = m_nop, ins.l.erase(), etc.
    return 1  # number of changes
```

**Block optimizer** (`optblock_t`): called per-block at MMAT_LOCOPT, MMAT_GLBOPT1, MMAT_GLBOPT2. Return count of changes.
```python
def optimize(blk):
    count = 0
    ins = blk.head
    while ins is not None:
        # Walk instruction chain, modify in-place
        if ins == blk.tail:
            break
        ins = ins.next
    return count
```

### Path B: ida-domain DecompilerHooks (via execute_python)

Use when you need hook callbacks at specific decompilation phases (locopt, glbopt, preoptimized) or direct `mba_t` access for complex graph transformations like CFF unflattening.

```python
from ida_domain import Database
from ida_domain.hooks import DecompilerHooks
import ida_hexrays

class MyHook(DecompilerHooks):
    def locopt(self, mba):
        if mba.maturity >= 6: return 0  # NEVER at MMAT_LVARS
        # Full mba_t access — iterate blocks, modify instructions
        return 0

    def preoptimized(self, mba):
        # Runs before local optimization
        return 0

    def glbopt(self, mba):
        # Runs during global optimization
        return 0

hook = MyHook()
with Database.open(hooks=[hook]) as db:
    ida_hexrays.mark_cfunc_dirty(func_ea)  # MUST invalidate cache
    lines = db.functions.get_pseudocode(func, remove_tags=True)
```

### When to Use Which

| Scenario | Path |
|---|---|
| Pattern-based simplification (opaque predicates, MBA, junk) | A — install_microcode_optimizer |
| NOP specific known-bad addresses | A — nop_microcode |
| CFF unflattening (need block-level CFG rewriting) | A (block optimizer) or B (for full mba_t access) |
| Complex multi-pass with state across blocks | B — ida-domain hooks |
| String decryption (xref walking + annotation) | A — built-in tools (xrefs_to, set_comment) |
| Symbolic solving (z3) | B — execute_python |

## Technique Rules

### CFF (Control Flow Flattening)

- Run at `MMAT_PREOPTIMIZED` or `MMAT_LOCOPT`. **Never** at `MMAT_LVARS` (maturity ≥ 6).
- State variable = highest constant comparison frequency — **no entropy filtering**. State values may not look random.
- Assume **multiple dispatchers** — any block comparing the state var is a dispatcher.
- Handler = non-dispatcher target for a given state value.
- After rewiring handlers, NOP all state variable assignments.
- Call `blk.mark_lists_dirty()` per modified block, `mba.mark_chains_dirty()` after all changes.
- Let Hex-Rays prune dead dispatcher blocks in the next locopt pass.

### Opaque Predicates

- Handle in optimizer callback: detect conditional jumps where one operand is a constant at decompile time.
- Always-true: force `m_goto` to the taken target.
- Always-false: `m_nop` the branch instruction.
- Hex-Rays automatically cleans dead blocks in the next locopt pass.
- For non-trivially-constant predicates (e.g., `x*(x-1)%2 == 0`), use z3 via `execute_python` to prove the predicate, then install an optimizer to force it.

### MBA (Mixed Boolean-Arithmetic)

- In optimizer callback: match `minsn_t` patterns where both operands are constants, calculate simplified value, overwrite in-place with `m_mov` + `make_number`.
- Apply `(1 << (size * 8)) - 1` bitmask when constant-folding to handle unsigned overflow.
- For symbolic MBA (operands aren't constants), consider z3 or pattern-matching known identities:
  - `(x ^ y) + 2*(x & y)` → `x + y`
  - `(x | y) - (x & ~y)` → `y`
  - `~(~x & ~y)` → `x | y`
  - `(x & 0xFF) | (x & ~0xFF)` → `x`

### Bogus Control Flow (BCF)

- OLLVM BCF adds conditional jumps with opaque predicates branching to junk blocks.
- Detect: block with an opaque predicate where one successor contains junk (no real data flow contribution).
- Remove: force the opaque predicate (same as opaque predicate removal), then dead block elimination handles junk.

### Instruction Substitution

- OLLVM replaces simple ops with equivalent complex sequences.
- Common patterns (match and reverse):
  - `a + b` → `a - (-b)`, `(a ^ b) + 2*(a & b)`, `(a | b) + (a & b)`
  - `a - b` → `a + (-b)`, `(a ^ b) - 2*(~a & b)`
  - `a ^ b` → `(a | b) - (a & b)`, `(~a & b) | (a & ~b)`
  - `a & b` → `(a | b) - (a ^ b)`, `~(~a | ~b)`
  - `a | b` → `(a & b) + (a ^ b)`, `~(~a & ~b)`
- Use instruction optimizer to pattern-match and replace.
- Multiple passes may be needed (substitutions can be chained).

### Dead Code / Junk Instructions

- Instructions computing values never used.
- In optimizer: if an instruction writes to a register/variable that has no uses before its next definition, NOP it.
- At microcode level: `m_mov` to a variable that is immediately overwritten → dead store → NOP.

### Anti-Disassembly

- Junk bytes after unconditional jumps confuse linear disassembly.
- Pattern: `jmp +2; db 0xE8` (fake call prefix).
- Fix at byte level via `execute_python`: `ida_bytes.patch_byte(ea, 0x90)` to NOP junk bytes, or redefine function boundaries.
- IDA usually handles this at the assembly level — check disassembly first.

## Critical Rules

| Rule | Detail |
|---|---|
| **Maturity guard** | ALWAYS check `mba.maturity >= 6` and return 0. Never modify at MMAT_LVARS or later |
| **Operand equality** | Always `a.equal_mops(b, EQ_IGNSIZE)` — never bare `.equal_mops(b)` |
| **In-place mutation** | Never instantiate new `minsn_t`. Overwrite opcodes and erase operands on existing ones |
| **Mark dirty** | After modifying a block: `blk.mark_lists_dirty()`. After a pass with changes: `mba.mark_chains_dirty()` |
| **NOP operands** | When NOPing: set `ins.opcode = m_nop`, then `ins.l.erase()`, `ins.r.erase()`, `ins.d.erase()` |
| **State variable** | No entropy filtering — find the variable with highest constant comparison frequency |
| **Dispatchers** | Assume multiple dispatcher blocks — any block comparing state var is a dispatcher |
| **Cache invalidation** | For ida-domain path: always call `mark_cfunc_dirty()` before decompiling |
| **Redecompile after** | After installing/removing optimizers, call `redecompile_function` to see the effect |
| **Iterative passes** | Complex obfuscation needs multiple passes — install optimizer, redecompile, check, refine |

## String Decryption

```
1. list_strings / search_strings → very few readable strings → encrypted
2. search_functions for small frequently-called functions → decode stub candidates
3. xrefs_to(decode_func) → all call sites
4. decompile_function(caller) → trace arguments to find encrypted data + key
5. execute_python → reimplement decode logic, compute plaintext
6. set_comment at each call site → "decrypted: <plaintext>"
7. rename_function(decode_func, "decrypt_string")
```

## Troubleshooting

**Optimizer never fires:**
- Did you call `redecompile_function()` after installing? The optimizer only runs during decompilation.
- Check `list_microcode_optimizers()` — is it registered?

**Crash / internal error after microcode edit:**
- You likely modified at MMAT_LVARS or later. Add maturity guard.
- Verify you're modifying in-place, not creating new `minsn_t` objects.

**CFF not detected:**
- Entropy filtering removed state variable candidates — remove any entropy filter.
- Check for multiple dispatchers; not all comparisons may be `m_jz`.
- State variable may be a memory operand (`mop_S` stack var) not a register (`mop_r`).

**State variable rewired but CFG still wrong:**
- Jump target was an address (`mop_v`/`mop_a`), not a block ref (`mop_b`). Convert with `make_blkref()`.

**Optimizer runs but no visible change:**
- Pattern may not match at the maturity level the optimizer fires at. Try reading microcode at different levels.
- The decompiler may re-optimize your changes away. Try a different approach or maturity level.
