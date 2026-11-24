# Binary Ninja Deobfuscation Tools

## IL Level Selection

| Level | What it shows | Best for |
|---|---|---|
| LLIL | 1:1 with machine instructions, flags, registers | Byte-level pattern matching, instruction-level junk detection |
| MLIL | Typed variables, no stack/flags, data flow computed | **Primary deobfuscation level** — state variable tracking, opaque predicates, CFF analysis |
| HLIL | C-like pseudocode, folded expressions | Verification after deobfuscation, high-level understanding |

**Default to MLIL for deobfuscation.** It abstracts away architecture differences while preserving enough structure for pattern matching. HLIL over-folds expressions (nested trees make pattern matching harder). LLIL is too low-level unless you need instruction-specific patterns.

## Reading

- `get_il(address, level)` — Read all IL instructions for a function. Use MLIL to see variables and data flow, LLIL for raw instruction patterns.
- `get_il_block(address, block_index, level)` — Read a single block's IL instructions with detail. Use after `get_il` to drill into specific blocks.
- `get_cfg(address, level)` — CFG structure: blocks, edges, back edges, dominators, loop headers. Essential for CFF analysis — identifies the dispatcher loop.
- `track_variable_ssa(address, variable_name, level)` — Trace a variable through SSA def-use chains. Every assignment, every use, every constant value. **Primary tool for finding state variables.**
- `decompile_function(address)` — Pseudocode output. Use after each modification to verify improvement.

## Writing (IL Level)

- `il_set_condition(address, condition_address, force, level)` — Force a conditional branch always-true or always-false. **Primary tool for opaque predicates and dispatcher branches.** Works by replacing the condition expression with a constant.
- `il_nop_expr(address, expr_index, level)` — NOP an IL expression by index. Use for state variable assignments, dead stores, junk computations.
- `il_replace_expr(address, expr_index, replacement_type, value, size, level)` — Replace an IL expression with a constant (`const`), NOP (`nop`), or copy from another index (`copy_from`). Use for MBA simplification — replace complex expression with simplified constant.
- `il_remove_block(address, block_index, level)` — NOP all instructions in a basic block. Use for dead/unreachable blocks after CFF or BCF removal.
- `redecompile_function(address)` — Force reanalysis and return current pseudocode. Call after every modification to verify improvement.

## Writing (Byte Level)

- `nop_instructions(func_address, instruction_addresses)` — NOP machine code bytes at given addresses. Architecture-aware (uses `convert_to_nop` when available). Use when IL-level modification is insufficient.
- `patch_branch(address, action)` — Force/invert/make unconditional a conditional branch at byte level. Actions: `force_true`, `force_false`, `invert`, `unconditional`. x86/x64 only. Fallback when `il_set_condition` fails.
- `write_bytes(address, hex_bytes)` — Write arbitrary bytes. Last resort.

## Batch Transform

- `install_il_workflow(name, description, il_level, python_code)` — Register a Python transform as a Binary Ninja workflow activity. Runs in the analysis pipeline for every function. Use when a pattern repeats across many functions (e.g., same opaque predicate template everywhere). The Python code must define a `transform(analysis_context, il_func)` function.

## Scripting (Last Resort)

- `execute_python(code)` — Run arbitrary Python with `bv`, `binaryninja`, `current_address` pre-loaded. Use for Z3 solving, complex multi-function analysis, emulation, or anything not covered by built-in tools.

## Decision Table

| Task | Preferred tool |
|---|---|
| Read IL to identify patterns | `get_il` (MLIL) |
| Understand CFG structure / find loops | `get_cfg` (MLIL) |
| Track state variable assignments | `track_variable_ssa` |
| Force opaque predicate | `il_set_condition` |
| NOP dead store / junk expression | `il_nop_expr` |
| Replace expression with constant | `il_replace_expr` (type=`const`) |
| Remove dead block | `il_remove_block` |
| NOP junk machine instructions | `nop_instructions` |
| Patch conditional branch (byte level) | `patch_branch` |
| Same deobfuscation across all functions | `install_il_workflow` |
| Z3 solving / emulation / complex logic | `execute_python` |
| Verify result after modifications | `decompile_function` or `redecompile_function` |
