---
name: smart-patch-binja
description: Patch binary code in Binary Ninja using natural language — read, assemble, write, verify
---
Task: Apply targeted binary patches in Binary Ninja based on the user's natural language description. Analyze the function, identify the minimal set of instructions to change, assemble new instructions, write them, and verify the result.

## Workflow

1. **Read** the target function's disassembly (`read_function_disassembly`) and decompiled pseudocode / `get_il` at HLIL level to understand its current behavior.

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
   # Assemble new instruction at the correct address
   new_bytes = bv.arch.assemble("jg 0x401300", 0x401248)
   original_size = 6  # from get_instruction_info

   # NOP padding if shorter
   if len(new_bytes) < original_size:
       nop = bv.arch.assemble("nop", 0)
       new_bytes += nop * (original_size - len(new_bytes))

   bv.write(0x401248, new_bytes)
   bv.update_analysis_and_wait()
   print(f"Patched {len(new_bytes)} bytes at 0x401248")
   ```

6. **Verify** with `redecompile_function` — confirm the HLIL output reflects the desired behavior change. If it doesn't match, revert by writing back the original bytes and try a different approach.

7. **Report** — If called from `/modify`, you MUST call:
   ```
   exploration_report(category="patch_result", address=..., summary="Patched X: old → new", original_hex="...", new_hex="...", evidence="redecompile confirms...")
   ```

8. **Annotate** each patched address with `set_comment` explaining what was changed and why.

## Safety Rules

- **In-memory only.** `bv.write()` modifies the in-memory BinaryView immediately. The `.bndb` file is only updated when the user does: File → Save or File → Save As.
- **When called from /modify**, do NOT call `bv.save()` — the Phase 4 save gate handles this.
- **Never exceed original boundaries.** New instructions must not be larger than the instructions they replace.
- **NOP padding is mandatory.** If new instructions are shorter, fill remaining bytes with NOPs.
- **Always back up first.** Print original bytes before writing any patch.
- **Always verify after.** Redecompile and confirm the change matches the user's intent.
- **Revert on failure.** If verification shows the patch didn't work: `bv.write(addr, original_bytes)` then `bv.update_analysis_and_wait()`.
- **Minimal changes only.** Patch the fewest bytes possible.

## NOP via IL

For single-instruction NOPs, prefer `nop_instructions` — it patches at the IL layer and triggers re-analysis. This is safer than `execute_python` for simple NOP operations because it handles alignment automatically.

## Common Patch Patterns

### Changing a conditional branch
Replace `jl` with `jg`, `je` with `jne`, etc. Same instruction size, just a different opcode byte.

### Inverting a condition
Change `test eax, eax` + `je` to `test eax, eax` + `jne`, or patch the comparison operand.

### Forcing a branch (always/never taken)
Replace conditional jump with `jmp` (always) or NOP out the jump (never).

### Changing an immediate operand
Reassemble the instruction with a new immediate value, e.g., `cmp eax, 0xa` → `cmp eax, 0x14`.

### Removing a check entirely
NOP out the comparison and conditional jump instructions using `nop_instructions`.
