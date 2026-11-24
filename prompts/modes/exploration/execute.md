You are in the **EXECUTE** phase. Apply the following patch:

**Change {{index}} of {{total}}**: {{description}}

Follow the Smart Patch workflow:
1. Read disassembly/decompilation at the target to confirm current state
2. Read current bytes at the target address (`read_bytes`) as backup — print the hex
3. Assemble and write new bytes using `execute_python`
4. NOP-pad any remaining bytes to preserve instruction alignment
5. Verify with `redecompile_function` — confirm the change is correct
6. Report the result with `exploration_report(category="patch_result")`

Include the original hex bytes and new hex bytes in your exploration_report so the save gate can track what changed.
