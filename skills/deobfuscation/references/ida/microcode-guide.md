# Microcode Reading & Writing Guide

## Reading Microcode via Rikugan Tools

```
get_microcode("0x401000", "MMAT_LOCOPT")
```

Returns formatted text showing all blocks, instructions, operands. Lower maturity = more raw detail (better for spotting obfuscation). Higher maturity = closer to final decompiler output.

```
get_microcode_block("0x401000", 3, "MMAT_LOCOPT")
```

Returns one block's instructions with full operand detail: types (`mop_r`, `mop_n`, `mop_b`, etc.), sizes, values. Use after `get_microcode` to drill into specific blocks.

## Maturity Levels

| Value | Name | Safe to Modify? | Best For |
|---|---|---|---|
| 0 | MMAT_GENERATED | ✅ Yes | Raw patterns, initial junk |
| 1 | MMAT_PREOPTIMIZED | ✅ Yes | CFF dispatchers still fully visible |
| 2 | MMAT_LOCOPT | ✅ **Best** | CFG ready, blocks structured, CFF + opaque predicates clear |
| 3 | MMAT_CALLS | ✅ Yes | Call arguments resolved |
| 4 | MMAT_GLBOPT1 | ✅ Yes | First global optimization |
| 5 | MMAT_GLBOPT2 | ✅ Yes | Most global optimization done |
| 6 | MMAT_GLBOPT3 | ❌ Never | Optimization fixed |
| 7 | MMAT_LVARS | ❌ **Never** | Local variables allocated — causes internal errors |

**Always guard with:** `if blk.mba.maturity >= 6: return 0` (in block optimizer) or check `ins` context.

## Microcode Structure

- `mba_t` contains `mblock_t` blocks (array indexed by `blk.serial`).
- Each `mblock_t` has a doubly-linked list of `minsn_t` instructions (`blk.head` → `blk.tail`).
- Each `minsn_t` has: `opcode` (m_mov, m_add, m_jz, m_goto...), `l` (left), `r` (right), `d` (destination) — all `mop_t`.

### Iterating a Block's Instructions

```python
ins = blk.head
while ins is not None:
    # inspect ins.opcode, ins.l, ins.r, ins.d
    if ins == blk.tail:
        break
    ins = ins.next
```

### Operand Types (mop_t)

| Type constant | Meaning | Access |
|---|---|---|
| `mop_z` | Empty/unused | — |
| `mop_r` | Register | `.r` (register number) |
| `mop_n` | Numeric constant | `.nnn.value` |
| `mop_b` | Block reference | `.b` (block index) |
| `mop_v` | Global/virtual address | `.g` (EA) |
| `mop_a` | Address | `.a` (EA) |
| `mop_S` | Stack variable | `.s.off` (stack offset) |
| `mop_d` | Nested instruction | `.d` (minsn_t) |
| `mop_f` | Call info | (function call details) |
| `mop_l` | Local variable | `.l.idx`, `.l.off` |

## Writing Microcode (via install_microcode_optimizer)

All optimizer code runs inside `install_microcode_optimizer`. The `ida_hexrays` namespace is pre-loaded with all constants.

### NOPing an Instruction

```python
def optimize(blk, ins):
    if should_nop(ins):
        ins.opcode = m_nop
        ins.l.erase()
        ins.r.erase()
        ins.d.erase()
        blk.mark_lists_dirty()
        return 1
    return 0
```

### Forcing a Goto (always-true branch)

```python
def optimize(blk, ins):
    if ins.opcode in (m_jz, m_jnz, m_jcnd) and is_always_taken(ins):
        ins.opcode = m_goto
        # ins.l already has the target; erase r and d
        ins.r.erase()
        ins.d.erase()
        blk.mark_lists_dirty()
        return 1
    return 0
```

### Replacing with a Constant (MBA fold)

```python
def optimize(blk, ins):
    fn = FOLD_OPS.get(ins.opcode)
    if fn and ins.l.t == mop_n and ins.r.t == mop_n:
        size = ins.d.size
        if size > 0:
            mask = (1 << (size * 8)) - 1
            result = fn(ins.l.nnn.value, ins.r.nnn.value) & mask
            ins.opcode = m_mov
            ins.l.make_number(result, size)
            ins.r.erase()
            blk.mark_lists_dirty()
            return 1
    return 0
```

### Rewiring a Block's Jump Target

```python
def optimize(blk):
    tail = blk.tail
    if tail and tail.opcode == m_goto:
        if tail.l.t == mop_b and tail.l.b == dispatcher_idx:
            tail.l.b = target_handler_idx
            blk.mark_lists_dirty()
            return 1
        elif tail.l.t in (mop_v, mop_a):
            # Address-based target — convert to block reference
            tail.l.make_blkref(target_handler_idx)
            blk.mark_lists_dirty()
            return 1
    return 0
```

### Comparing Operands

```python
# ALWAYS use EQ_IGNSIZE. Never bare .equal_mops(b).
EQ_IGNSIZE = ida_hexrays.EQ_IGNSIZE
if a.equal_mops(b, EQ_IGNSIZE):
    ...
```

### Copying an Operand

```python
def copy_mop(src):
    r = ida_hexrays.mop_t()
    r.assign(src)
    return r
```

### After Any Modification

```python
blk.mark_lists_dirty()       # After modifying any block
# In block optimizer, if you made cross-block changes:
# mba.mark_chains_dirty()    # (access via blk.mba if available)
```

## All Conditional Jump Opcodes

```python
_COND_OPCODES = {
    m_jcnd, m_jz, m_jnz,
    m_jg, m_jl, m_jge, m_jle,
    m_ja, m_jae, m_jb, m_jbe,
}
```

## Visitor Pattern (via execute_python only)

When you need to scan the entire MBA (e.g., to find the state variable), use `execute_python`:

```python
class MyVisitor(ida_hexrays.minsn_visitor_t):
    def __init__(self):
        ida_hexrays.minsn_visitor_t.__init__(self)
        self.results = []

    def visit_minsn(self):
        ins = self.curins
        blk = self.blk
        # inspect ins, blk
        return 0  # 0 = continue visiting

v = MyVisitor()
mba.for_all_topinsns(v)
```

## Getting an Instruction's Address

```python
def get_instruction_ea(ins):
    if hasattr(ins, 'ea'):
        return ins.ea
    if hasattr(ins, 'ip'):
        return ins.ip
    return None
```

## Byte-Level Patching (last resort)

When microcode-level modification is insufficient, use `execute_python`:

```python
ida_bytes.patch_byte(ea, 0x90)           # NOP one byte
ida_bytes.patch_bytes(ea, b"\x90" * 5)   # NOP sled
```

Or through ida-domain:
```python
with Database.open() as db:
    db.bytes.patch_byte_at(ea, value)
```

## Key Microcode Opcodes

| Category | Opcodes |
|----------|---------|
| Data movement | `m_mov`, `m_ldc`, `m_stx`, `m_ldx` |
| Arithmetic | `m_add`, `m_sub`, `m_mul`, `m_udiv`, `m_sdiv`, `m_umod`, `m_smod` |
| Bitwise | `m_and`, `m_or`, `m_xor`, `m_shl`, `m_shr`, `m_sar`, `m_bnot` |
| Comparison/set | `m_setz`, `m_setnz`, `m_setb`, `m_seta`, `m_setl`, `m_setg` |
| Control flow | `m_jcnd`, `m_jz`, `m_jnz`, `m_jg`, `m_jl`, `m_jge`, `m_jle`, `m_ja`, `m_jae`, `m_jb`, `m_jbe` |
| Unconditional | `m_goto`, `m_call`, `m_ret` |
| Special | `m_nop`, `m_jtbl` |
