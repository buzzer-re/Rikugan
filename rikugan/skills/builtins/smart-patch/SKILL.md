---
name: Smart Patch
description: Modify binary code using natural language — analyze, assemble, and patch
tags: [patching, assembly, binary, modification]
author: Rikugan
version: 1.0
allowed_tools:
  - read_disassembly
  - read_function_disassembly
  - get_instruction_info
  - decompile_function
  - get_pseudocode
  - read_bytes
  - get_il
  - execute_python
  - redecompile_function
  - nop_instructions
  - set_comment
---
Task: Apply targeted binary patches based on the user's natural language description. Analyze the function, identify the minimal set of instructions to change, assemble new instructions, write them, and verify the result.

## Workflow

1. **Read** the target function's disassembly (`read_function_disassembly`) and decompiled pseudocode (`decompile_function`) to understand its current behavior.

2. **Identify** which specific instructions implement the behavior the user wants to change. Use `get_instruction_info` to get exact byte sizes and encodings for the target instructions.

3. **Back up** the original bytes before patching. Use `read_bytes` at the target address for the instruction length, and print them so the user has a record:
   ```
   Original bytes at 0x{addr:x}: {hex_bytes}
   ```

4. **Plan** the minimal patch:
   - Determine what new instruction(s) achieve the desired behavior.
   - Ensure the new instructions fit within the original byte boundaries.
   - If new instructions are shorter, the remaining bytes MUST be filled with NOPs.
   - Verify branch targets and relative offsets are correct for the patch address.

5. **Patch** using `execute_python` with Binary Ninja's assembler and writer:
   ```python
   # Assemble the new instruction at the correct address
   new_bytes = bv.arch.assemble("jg 0x{target:x}", 0x{addr:x})
   original_size = {size}  # size of the original instruction(s)

   # Pad with NOPs if shorter
   if len(new_bytes) < original_size:
       nop = bv.arch.assemble("nop", 0)
       new_bytes += nop * (original_size - len(new_bytes))

   # Write the patch
   bv.write(0x{addr:x}, new_bytes)
   bv.update_analysis_and_wait()
   print(f"Patched {len(new_bytes)} bytes at 0x{addr:x}")
   ```

6. **Verify** with `redecompile_function` — confirm the decompiled output reflects the desired behavior change. If it doesn't match, revert by writing back the original bytes and try a different approach.

7. **Annotate** each patched address with `set_comment` explaining what was changed and why.

## Safety Rules

- **Never exceed original boundaries.** New instructions must not be larger than the instructions they replace. If they don't fit, find an alternative encoding or a different patching strategy.
- **NOP padding is mandatory.** If new instructions are shorter than the originals, fill remaining bytes with NOPs to preserve alignment.
- **Always back up first.** Print original bytes before writing any patch.
- **Always verify after.** Redecompile and confirm the change matches the user's intent.
- **Revert on failure.** If verification shows the patch didn't work, write back the original bytes, then try again with a different approach.
- **Minimal changes only.** Patch the fewest bytes possible. Don't rewrite entire functions when changing one comparison suffices.

## Common Patch Patterns

### Changing a conditional branch
Replace `jl` with `jg`, `je` with `jne`, etc. Same instruction size, just a different opcode byte.

### Inverting a condition
Change `test eax, eax` + `je` to `test eax, eax` + `jne`, or patch the comparison operand.

### Forcing a branch (always/never taken)
Replace conditional jump with `jmp` (always) or `nop` out the jump (never).

### Changing an immediate operand
Reassemble the instruction with a new immediate value, e.g., `cmp eax, 0xa` → `cmp eax, 0x14`.

### Removing a check entirely
NOP out the comparison and conditional jump instructions using `nop_instructions`.
