# IDA Pro Deobfuscation Tools

## Reading

- `decompile_function` — Read decompiled pseudocode. After each modification, redecompile to verify improvement.
- `get_microcode` — Read microcode at any maturity level (MMAT_GENERATED through MMAT_LVARS). Lower levels expose obfuscation patterns; higher levels show optimized output. Use MMAT_LOCOPT or MMAT_PREOPTIMIZED to spot CFF dispatchers, opaque predicates, and junk instructions.
- `get_microcode_block` — Read a single block's instructions with full operand detail (types, sizes, values). Use after get_microcode to drill into specific blocks.
- `redecompile_function` — Force redecompilation with `mark_cfunc_dirty()`. Call after installing/removing optimizers to see the effect.
- `read_disassembly` / `read_function_disassembly` — Read raw assembly. Useful for byte-level pattern detection and verifying patches.
- `xrefs_to` / `xrefs_from` / `function_xrefs` — Trace cross-references. Essential for finding decode stub call sites during string decryption.
- `list_strings` / `search_strings` — Find readable strings. Few strings in a large binary → strings are encrypted.
- `list_functions` / `search_functions` — Find functions by name or pattern. Locate decode stubs, VM handlers, dispatchers.
- `get_binary_info` / `list_segments` / `list_imports` — Binary metadata. Identify packed sections, unusual segments, import obfuscation.

## Writing (Microcode Level)

- `nop_microcode` — NOP specific microcode instructions by address. Installs a persistent optimizer that replaces targeted instructions with `m_nop` during decompilation. Use for junk instructions, dead stores, state variable assignments.
- `install_microcode_optimizer` — Install a custom Python optimizer that runs during every subsequent decompilation. Two types:
  - **instruction** (`optinsn_t`): `def optimize(blk, ins) -> int` — called per-instruction. Use for pattern matching, opaque predicates, MBA constant folding, instruction substitution reversal.
  - **block** (`optblock_t`): `def optimize(blk) -> int` — called per-block. Use for CFF unflattening, dead block removal, jump rewiring.
  - All `ida_hexrays` constants available: `m_nop`, `m_mov`, `m_goto`, `m_jz`, `m_jnz`, `mop_r`, `mop_n`, `mop_b`, `mop_v`, etc.
- `remove_microcode_optimizer` — Remove a previously installed optimizer by name.
- `list_microcode_optimizers` — Show all active optimizers with descriptions and applied counts.

## Writing (Annotation & Database)

- `rename_function` / `rename_variable` / `rename_address` — Annotate deobfuscated code with meaningful names.
- `set_comment` — Add comments (e.g., decrypted string values at call sites).
- `set_type` / `set_function_prototype` / `create_struct` — Apply recovered types after deobfuscation.

## Scripting (Last Resort)

- `execute_python` — Run arbitrary IDAPython code. Use ONLY when built-in tools are insufficient:
  - Bulk operations across hundreds of functions
  - Complex computations (z3 solver, crypto reimplementation)
  - `ida-domain` Database/DecompilerHooks for hook-based deobfuscation that needs locopt/glbopt/preoptimized callbacks
  - Direct microcode manipulation needing `mba_t`/`mblock_t`/`minsn_t` access beyond what optimizers provide
  - All `ida_*` modules, `idaapi`, `idautils`, `idc`, and `ida-domain` are available.

## Choosing the Right Approach

| Task | Preferred Tool | Why |
|---|---|---|
| Spot obfuscation patterns | `get_microcode` at MMAT_LOCOPT | See raw dispatcher structure before optimization removes evidence |
| NOP known junk addresses | `nop_microcode` | One-shot, persistent, immediate redecompile |
| Pattern-match & replace across all functions | `install_microcode_optimizer` (instruction) | Runs automatically on every decompile |
| CFF unflattening | `install_microcode_optimizer` (block) | Needs block-level CFG access |
| Opaque predicate removal | `install_microcode_optimizer` (instruction) | Match constant-conditioned jumps |
| MBA constant folding | `install_microcode_optimizer` (instruction) | Match binary ops on two constants |
| String decryption (xref-based) | `xrefs_to` + `decompile_function` + `set_comment` | Built-in tools cover the workflow |
| Complex decode reimplementation | `execute_python` | Need full Python for crypto logic |
| Hook-based deobfuscation (ida-domain) | `execute_python` with `Database.open(hooks=[...])` | Need DecompilerHooks callbacks |
