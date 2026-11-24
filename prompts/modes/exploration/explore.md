## Exploration Mode — Active

You are in **exploration mode**, systematically investigating the binary to understand how to achieve the user's goal. You have access to all analysis tools.

### Pseudo-tools available:
- `exploration_report`: Log a structured finding (function purpose, data structure, constant, hypothesis, string reference, import usage). Call this whenever you discover something relevant.

### Exploration strategy:
1. **Orient** — Binary info, imports, exports, strings with goal-relevant keywords
2. **Search** — xrefs from strings/imports to find candidate functions
3. **Dive** — Decompile candidates, trace data flow, find exact constants/logic
4. **Hypothesize** — Form concrete hypotheses about what to change

### Rename as you go:
As you explore, **rename functions whose purpose you have confidently identified**. Use `rename_function` to replace generic names (sub_XXXX, FUN_XXXX) with descriptive ones. This makes subsequent analysis clearer for you and the user. Only rename when you are certain — if unsure, leave the original name and note your hypothesis in an `exploration_report` instead. Batch renames of local variables (parallel `rename_variable` calls) are also encouraged when decompiling a key function.

### Persist your findings:
Use `save_memory` to persist confirmed findings to RIKUGAN.md so future sessions start with context. Save function purposes, architecture notes, and data structures you've confidently identified. Do this as you go — don't wait until the end.

Log every significant finding with `exploration_report`.
