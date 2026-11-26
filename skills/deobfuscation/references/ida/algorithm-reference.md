# Algorithm Reference

Methodology and recognition patterns for each deobfuscation technique. This reference teaches you **how to identify and adapt** — not copy-paste solutions. Every binary is different.

**Workflow for every technique:** Read microcode → Identify the specific variant → Design a targeted fix → Install optimizer → Redecompile → Verify → Iterate.

---

## CFF — Control Flow Flattening Removal

### Recognition

Read the microcode at `MMAT_LOCOPT` or `MMAT_PREOPTIMIZED`. CFF has these signatures:

- **State variable**: One variable compared against many constants across multiple blocks. Find it by counting — the variable with the highest constant-comparison frequency across all conditional jumps IS the state variable. **No entropy filtering** — state values may be sequential, small, or otherwise "non-random."
- **Dispatcher blocks**: Any block that compares the state variable is a dispatcher. There may be **multiple dispatchers** (not just one central switch).
- **Handler blocks**: The non-dispatcher targets of state comparisons. Each handler sets the next state value and jumps back to a dispatcher.
- **Back edges**: Handlers end with `m_goto` back to a dispatcher block.

At low maturity, the state variable may appear as a **memory store** (`[rbp-0x10].d = 0x25`) rather than a register assignment. Check both `mop_r` and `mop_S` operand types.

### Approach

1. **Find the state variable.** Walk all instructions with a visitor (`mba.for_all_topinsns`). For each conditional jump (`m_jz`, `m_jnz`, etc.) where one operand is a constant (`mop_n`), record the other operand. Group by operand equality (`equal_mops` with `EQ_IGNSIZE`). The operand group with the most comparisons is the state variable.

2. **Identify all dispatchers.** Every block that compares the state variable is a dispatcher. Collect the set of dispatcher block indices.

3. **Map states to handlers.** For each comparison `state_var == C` in a dispatcher, the jump target that is NOT a dispatcher is the handler for state `C`. Resolve targets — they may be block refs (`mop_b`), addresses (`mop_v`/`mop_a`), or require an address-to-block lookup.

4. **Find state assignments.** Walk all `m_mov` instructions where the destination equals the state variable and the source is a constant. Map `block_index → [assigned_state_values]`. A handler block that assigns exactly one state value tells you its successor.

5. **Rewire handlers.** For each handler that assigns state `S` and jumps to a dispatcher: change its `m_goto` target to point directly to the handler for state `S`. This bypasses the dispatcher.

6. **NOP state assignments.** After rewiring, the `m_mov`s that set the state variable are dead — NOP them.

7. **Clean up.** Call `mark_lists_dirty()` on each modified block, `mark_chains_dirty()` on the MBA. Redecompile. Hex-Rays prunes unreachable dispatcher blocks automatically.

### Key decisions you must make per-binary

- **Which maturity level?** `MMAT_PREOPTIMIZED` preserves the most structure. `MMAT_LOCOPT` has cleaner blocks. Read at both and compare.
- **Block optimizer vs ida-domain hook?** Block optimizer sees one block at a time — good for NOP'ing state assignments. For the full rewiring (which needs cross-block state mapping), use `execute_python` with an ida-domain `DecompilerHooks.locopt(mba)` callback that has full MBA access.
- **Address vs block targets?** Jump targets may be raw addresses, not block indices. Build an address-to-block map first. If a `m_goto` target is `mop_v`/`mop_a`, resolve it and use `make_blkref()` to convert.
- **Multiple dispatchers?** Don't assume a single switch block. Some CFF variants chain dispatchers or split the state space across multiple comparison blocks.

### Idioms

```python
# Iterate all instructions via visitor (for state variable search)
class MyVisitor(ida_hexrays.minsn_visitor_t):
    def visit_minsn(self):
        ins = self.curins
        # self.blk is the containing block
        return 0
mba.for_all_topinsns(visitor)

# Rewire a goto to point to a different block
tail.l.make_blkref(target_block_index)
blk.mark_lists_dirty()

# Check if a jump target resolves to a dispatcher
if tail.l.t == mop_b:
    target_idx = tail.l.b
elif tail.l.t in (mop_v, mop_a):
    addr = tail.l.g if tail.l.t == mop_v else tail.l.a
    target_idx = find_block_by_addr(addr)
```

---

## Opaque Predicates

### Recognition

- **Trivially constant**: A conditional jump where the tested operand is already `mop_n` (numeric constant). The decompiler didn't fold it — likely injected after initial optimization.
- **Algebraically constant**: Expressions like `x*(x-1) % 2 == 0` (always true), `x^2 + x` is always even. These require symbolic reasoning.
- **Environment-based**: Checks on values constant at analysis time (e.g., PEB fields, hardcoded globals).

### Approach

**For trivially constant predicates** — install an instruction optimizer:
- Match conditional jumps where `ins.l.t == mop_n`.
- If the value is nonzero (true): convert to `m_goto` targeting the branch destination.
- If zero (false): NOP the instruction (fall through).
- Note: for `m_jz`/`m_jnz`, the branch semantics differ — `m_jz` branches when zero, `m_jnz` when nonzero. Adapt your true/false logic accordingly.

**For algebraically constant predicates** — use Z3 via `execute_python`:
- Extract the expression from the microcode.
- Model it as a Z3 formula.
- Ask Z3 to find a counterexample (`solver.add(Not(predicate))`).
- If `unsat` → always true. If `sat` with `predicate` itself → always false.
- Then install an optimizer that forces that specific pattern.

**Key decision:** Don't try to match every possible algebraic identity by hand. Use Z3 for anything non-trivial. The agent should read the microcode, extract the expression, and let the solver prove it.

### Idioms

```python
# Force a conditional jump to always-taken (goto)
ins.opcode = m_goto
if ins.d.t in (mop_b, mop_v, mop_a):
    ins.l.assign(ins.d)  # move target to goto's operand
ins.r.erase()
ins.d.erase()
blk.mark_lists_dirty()

# Z3 proof sketch
x = z3.BitVec('x', 32)
solver = z3.Solver()
solver.add(z3.Not(expression_always_true))
assert solver.check() == z3.unsat  # proven
```

---

## MBA — Mixed Boolean-Arithmetic

### Recognition

Simple operations disguised as complex boolean-arithmetic expressions. Look for sequences of `m_xor`, `m_and`, `m_or`, `m_add` that operate on the same variables — especially when the result of one feeds into the next.

### Approach

**Constant folding** (both operands are `mop_n`): Compute the result, replace with `m_mov` + `make_number`. Apply `(1 << (size * 8)) - 1` bitmask to handle unsigned overflow. This handles cases where the obfuscator pre-computed partial results as constants.

**Symbolic simplification** (operands are variables): Pattern-match known identities. Common ones to check:

| Obfuscated form | Simplified |
|---|---|
| `(x ^ y) + 2*(x & y)` | `x + y` |
| `(x \| y) + (x & y)` | `x + y` |
| `(x \| y) - (x & ~y)` | `y` |
| `~(~x & ~y)` | `x \| y` |
| `~(~x \| ~y)` | `x & y` |
| `(x & M) \| (x & ~M)` | `x` (identity mask) |

**Key decisions:**
- Work bottom-up — simplify inner nested expressions first.
- Multiple passes are usually needed since MBA can be layered.
- For identities not in the table above, use Z3 to prove equivalence: model both the complex and simple form, check if they're equal for all inputs.
- Constant folding should map the standard arithmetic/bitwise opcodes: `m_add`, `m_sub`, `m_xor`, `m_and`, `m_or`, `m_mul`, `m_shl`, `m_shr`.

### Idioms

```python
# Constant fold pattern in instruction optimizer
fn = FOLD_TABLE.get(ins.opcode)
if fn and ins.l.t == mop_n and ins.r.t == mop_n:
    size = ins.d.size
    mask = (1 << (size * 8)) - 1
    result = fn(ins.l.nnn.value, ins.r.nnn.value) & mask
    ins.opcode = m_mov
    ins.l.make_number(result, size)
    ins.r.erase()
    blk.mark_lists_dirty()
```

---

## Instruction Substitution Reversal

### Recognition

OLLVM replaces simple arithmetic/logic ops with equivalent complex sequences. The substituted form computes the same result using more instructions.

### Common substitution patterns (match and reverse)

| Original | Substituted forms |
|---|---|
| `a + b` | `a - (-b)`, `(a ^ b) + 2*(a & b)`, `(a \| b) + (a & b)` |
| `a - b` | `a + (-b)`, `(a ^ b) - 2*(~a & b)` |
| `a ^ b` | `(a \| b) - (a & b)`, `(~a & b) \| (a & ~b)` |
| `a & b` | `(a \| b) - (a ^ b)`, `~(~a \| ~b)` |
| `a \| b` | `(a & b) + (a ^ b)`, `~(~a & ~b)` |

### Approach

- **Simple single-instruction substitutions** (e.g., `sub x, -C` instead of `add x, C`): Match in an instruction optimizer. Check if the constant operand is a negation and reverse it.
- **Multi-instruction substitutions** (e.g., the 3-instruction `(a ^ b) + 2*(a & b)` form): These span multiple instructions. Use a block-level visitor to detect instruction sequences that feed into each other. Match by checking def-use chains — does one instruction's destination feed into the next instruction's operand?
- **Chained substitutions**: Substitutions may be nested. Run multiple passes.

**Key decision:** Don't try to enumerate every pattern. Focus on what you see in the specific binary. Read the microcode, spot the repeated complex sequences, identify what simple operation they implement, then write a targeted optimizer.

---

## Dead Code / Junk Instructions

### Recognition

- Self-moves: `m_mov reg, reg` (same register, same size)
- Self-XOR: `m_xor reg, reg` → always produces 0 (junk unless result is used)
- Add/sub zero: `m_add x, 0` or `m_sub x, 0`
- Multiply by 1: `m_mul x, 1`
- Dead stores: A variable is written but never read before being overwritten again

### Approach

The simple patterns (self-move, add 0, mul 1) can be matched per-instruction in an optimizer. For dead store elimination, you'd need def-use analysis across the block — check if an instruction's destination has any uses before its next definition.

**After CFF removal**, many state variable assignments become dead. A cleanup pass that NOPs any `m_mov` to the former state variable is effective.

---

## Bogus Control Flow (BCF)

BCF is opaque predicates + junk blocks. The removal is the same as opaque predicate elimination — once the predicate is forced, the junk block becomes unreachable, and Hex-Rays prunes it automatically. No separate algorithm needed.

---

## Anti-Disassembly

Byte-level problem, byte-level fix. Use `execute_python`.

### Common patterns

- **Junk after unconditional jump**: `jmp +2; db 0xE8` — the `0xE8` (CALL prefix) causes the disassembler to misparse.
- **Overlapping instructions**: A short jump lands in the middle of a multi-byte instruction.

### Fix

```python
import ida_bytes
ida_bytes.patch_byte(junk_ea, 0x90)         # NOP one junk byte
ida_bytes.patch_bytes(start, b"\x90" * n)    # NOP range
# Then redefine the function if needed:
# ida_funcs.del_func(func_ea); idc.add_func(func_ea)
```

Check disassembly view first — IDA often handles these at the disassembly level already.

---

## VM Boundary Detection

Not reversible with IL-level tools. Document and move on.

1. **Identify**: VM entry point (where native code transfers control to the VM), bytecode buffer address, handler table address, handler dispatch mechanism.
2. **Document**: Use `set_comment` and `rename_address` to annotate all VM-related addresses.
3. **Scope**: Focus deobfuscation efforts on non-virtualized code paths.
4. **Escalate**: Recommend specialized VM lifting tools (Miasm, angr, Triton) for the virtualized portions.

---

## Combining Techniques

Most real obfuscation uses multiple layers. The agent should:

1. **Read first** — get microcode at multiple maturity levels to understand what's present.
2. **Prioritize** — CFF is usually the outermost layer. Remove it first to expose inner layers.
3. **Install incrementally** — one optimizer at a time. Redecompile after each. Don't try to fix everything in one pass.
4. **Verify after each step** — if the code doesn't improve, the pattern match was wrong. Read again, adjust.
5. **Layer removal order**: typically CFF → opaque predicates/BCF → MBA → instruction substitution → dead code cleanup.

For `execute_python` with ida-domain hooks, multiple techniques can run in a single `DecompilerHooks` subclass — call each pass function from `locopt(mba)` or `preoptimized(mba)`. But only combine passes after each one works individually.
