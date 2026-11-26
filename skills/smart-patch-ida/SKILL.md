---
name: smart-patch-ida
description: Patch binary code in IDA Pro using natural language — read, assemble, write, verify
---
Task: Apply targeted binary patches in IDA Pro based on the user's natural language description. Analyze the function, identify the minimal set of instructions to change, assemble new instructions, write them, and verify the result.

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

5. **Patch** using `execute_python` with IDA's byte-patching API:
   ```python
   import ida_bytes, idc

   # Option A: manual opcode (for simple patches like branch inversion)
   ida_bytes.patch_bytes(0xADDR, bytes([0x75]))  # JNZ instead of JZ

   # Option B: use keystone assembler (if installed)
   import keystone
   ks = keystone.Ks(keystone.KS_ARCH_X86, keystone.KS_MODE_64)
   encoding, _ = ks.asm("jg 0x401300", 0x401248)
   ida_bytes.patch_bytes(0x401248, bytes(encoding))

   # NOP padding
   remaining = original_size - len(encoding)
   if remaining > 0:
       ida_bytes.patch_bytes(0x401248 + len(encoding), bytes([0x90] * remaining))
   print(f"Patched at 0x401248")
   ```

6. **Verify** with `redecompile_function` — confirm the decompiled output reflects the desired behavior change. If it doesn't match, revert by writing back the original bytes and try a different approach.

7. **Report** — If called from `/modify`, you MUST call:
   ```
   exploration_report(category="patch_result", address=..., summary="Patched X: old → new", original_hex="...", new_hex="...", evidence="redecompile confirms...")
   ```

8. **Annotate** each patched address with `set_comment` explaining what was changed and why.

## Safety Rules

- **IDB only.** Patches are applied to the IDB analysis database, not the binary file on disk. The binary file is only modified when the user does: File → Produce file → Create patched file.
- **When called from /modify**, do NOT save to file — the Phase 4 save gate handles this.
- **Never exceed original boundaries.** New instructions must not be larger than the instructions they replace.
- **NOP padding is mandatory.** If new instructions are shorter, fill remaining bytes with NOPs (`0x90`).
- **Always back up first.** Print original bytes before writing any patch.
- **Always verify after.** Redecompile and confirm the change matches the user's intent.
- **Revert on failure.** If verification shows the patch didn't work: `ida_bytes.patch_bytes(addr, original_bytes)`.
- **Minimal changes only.** Patch the fewest bytes possible.

## NOP via Microcode

For obfuscation cleanup, prefer `nop_microcode` to suppress instructions at the Hex-Rays IR level without touching bytes — useful when byte-level NOP would affect alignment or when the goal is to remove a check from the decompiler output only.

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
NOP out the comparison and conditional jump instructions using `nop_microcode`.
