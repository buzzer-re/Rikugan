---
name: modify
description: Modify binary behavior using natural language — explore, plan, patch, save
mode: exploration
---
Task: Modify the binary's behavior based on the user's natural language description. You will autonomously explore the binary to understand it, formulate a concrete plan, and apply minimal patches.

## Phase 1: Exploration Strategy

Your goal is to build enough understanding of the binary to know WHERE and HOW to make the requested change. Use `exploration_report` to log every significant finding.

### Step 1: Orientation (1-2 turns)
- `get_binary_info` — architecture, format, size
- `list_imports` + `list_exports` — what APIs does the binary use?
- `search_strings` with keywords from the user's request
  - For a game mod request mentioning "snake", search for: "snake", "score", "point", "length", "size", "spawn", "init", "level", "life", "speed"
  - Cast a wide net with goal-relevant keywords

### Step 2: Targeted Search (2-5 turns)
- From string hits, use `xrefs_to` to find which functions reference them
- From import hits, use `xrefs_to` to find call sites
- `search_functions` for names containing relevant keywords
- Build a shortlist of candidate functions
- **Log each candidate** with `exploration_report(category="function_purpose")`

### Step 3: Deep Dive (3-10 turns)
- `decompile_function` on the most promising candidates
- Trace data flow: where does the target value come from? Where is it used?
- Identify exact instructions and constants that control the behavior
- Use `get_il` for detailed intermediate representation when needed
- **Form concrete hypotheses** and log with `exploration_report(category="hypothesis")`
  - Example: "Changing the constant 3 at 0x401248 to 6 would double the snake's initial length"
  - Example: "Multiplying the score increment at 0x4015C2 by 2 would double points"

### Step 4: Transition Decision
- When you have identified ALL locations that need to change, finish your turn with a summary of your findings and no further tool calls; the plugin then moves to the plan phase
- If you're stuck or the binary is too complex, use `ask_user` to get hints
- Don't transition too early — make sure you understand the full picture

## Phase 2: Planning Guidelines

When you transition to the PLAN phase, you will receive a synthesis prompt with your accumulated findings. Create a numbered plan where each step specifies:

1. **Exact address** to modify (hex)
2. **Current behavior** at that address (what the code does now)
3. **Desired behavior** (what it should do after the patch)
4. **Patch strategy** (which bytes/instructions to change, new values)

Be precise. The plan will be shown to the user for approval before execution.

Example format:
```
1. Change snake initial length constant at 0x401248 from 3 to 6 (mov eax, 3 -> mov eax, 6)
2. Double score increment at 0x4015C2: change `add [score], 10` to `add [score], 20`
```

## Phase 3: Execution

The agent host determines which patching skill to use:
- **IDA Pro**: read skill://smart-patch-ida with the read tool
- **Binary Ninja**: read skill://smart-patch-binja with the read tool

The host is visible in the system prompt context. Read the correct skill at
the start of Phase 3, then follow its workflow for each planned change.

After each patch, you MUST call:
```
exploration_report(category="patch_result", address=..., summary="...", original_hex="...", new_hex="...")
```

This is required for the Phase 4 save gate to know what was applied.

### Safety Rules
- Never exceed original instruction boundaries
- NOP-pad if new instructions are shorter
- Always backup before patching
- Always verify after patching
- Revert on failure (write back original bytes)
- Minimal changes only — patch the fewest bytes possible
