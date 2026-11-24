---
name: deobfuscation
description: >-
  Systematic binary deobfuscation — string decryption, control flow flattening (CFF) removal,
  opaque predicate elimination, mixed boolean-arithmetic (MBA) simplification, bogus control flow,
  instruction substitution reversal, dead code removal, and anti-disassembly fixes.
  Trigger: deobfuscate, unobfuscate, deobfuscation, CFF, flatten, opaque predicate, MBA,
  obfuscated, OLLVM, Tigress, VMProtect, string decryption, junk code, bogus control flow,
  instruction substitution, anti-disassembly
mode: plan
---
# Deobfuscation Mode

Deobfuscate fully first. Analyze afterward.
**Never draw conclusions from obfuscated code — it misleads.**

Host-specific tools, API details, and full algorithm implementations are in the references:

- **IDA**: `references/ida/tools.md` (available tools), `guide.md` (workflow & technique rules), `microcode-guide.md` (reading/writing microcode), `algorithm-reference.md` (recognition & methodology)
- **Binary Ninja**: `references/binja/tools.md` (available tools), `guide.md` (workflow & technique rules), `il-guide.md` (reading/writing BNIL), `algorithm-reference.md` (recognition & methodology)

When running in IDA Pro, read these with the read tool before starting: skill://deobfuscation/references/ida/tools.md, skill://deobfuscation/references/ida/guide.md, skill://deobfuscation/references/ida/microcode-guide.md, skill://deobfuscation/references/ida/algorithm-reference.md.

When running in Binary Ninja, read these with the read tool before starting: skill://deobfuscation/references/binja/tools.md, skill://deobfuscation/references/binja/guide.md, skill://deobfuscation/references/binja/il-guide.md, skill://deobfuscation/references/binja/algorithm-reference.md.

**Read the host-specific tools reference first** to know what primitives are available.

## Order of Operations

### 1. String Decryption (always first — unlocks context)

Skip only if the user asked for a specific deobfuscation type.

Strings are the fastest path to understanding a binary. Encrypted strings signal intentional obfuscation.

1. Check for readable strings — very few in a large binary means strings are encrypted.
2. Find the decode stub: a small function called frequently, often before string use.
3. Decompile it and identify the algorithm (XOR, RC4, custom).
4. Use xrefs on the decode function to locate all call sites.
5. At each call site, trace arguments to extract encrypted data and key.
6. Reimplement the decode logic to compute plaintext.
7. Annotate decrypted strings at each call site (C2 addresses, file paths, registry keys, API names).
8. Rename the decode function (e.g., `decrypt_string`).

### 2. Structural Deobfuscation

**You are the deobfuscator.** Read the IL/pseudocode, understand the obfuscation, then use write primitives to undo it. Read, understand, modify, verify.

Work through these techniques in order — each one may reveal patterns hidden by the previous layer.

#### Control Flow Flattening (CFF)

A state machine dispatcher replaces linear control flow.

**What it looks like:**
- A loop with a back edge to a header block.
- The header compares a variable (state variable) against many constants.
- Each case body assigns a new constant to the state variable.
- At low optimization: state var may appear as memory stores (`[rbp-0x10].d = 0x25`), not register vars.

**How to remove:**
1. Identify the state variable — the variable compared most frequently against constants. **No entropy filtering** — state values may not look random.
2. Assume **multiple dispatcher blocks** — any block comparing the state variable is a dispatcher.
3. Map state to handler: the non-dispatcher jump target for each state value.
4. Rewire each handler to jump directly to its successor handler (bypass dispatcher).
5. NOP all state variable assignments (dispatcher bookkeeping).
6. Remove dead dispatcher blocks (the decompiler handles this automatically after rewiring).
7. Redecompile — the function should read as linear code.

#### Opaque Predicates

Conditions that always evaluate the same way.

**Forms:**
- Algebraic: `x * (x-1) % 2 == 0` (always true), `x^2 + x` is always even.
- Constant-returning functions hidden behind indirection.
- Environment checks that are constant at analysis time.

Check the decompiler output first — it may already resolve some to `if (true)`/`if (false)`.

**How to remove:**
- Always-true: force the branch to the taken target (convert to unconditional goto).
- Always-false: NOP the branch instruction entirely.
- The dead branch becomes unreachable — the decompiler prunes it automatically.
- For non-trivially-constant predicates, use Z3 via scripting to prove the predicate, then install an optimizer/transform to force it.

#### Bogus Control Flow (BCF)

Conditional jumps with opaque predicates branching to junk/cloned blocks. Common in OLLVM.

**What it looks like:**
- A conditional branch where one successor contains junk code (no real data flow contribution) or a clone of the original block with added noise.
- The opaque predicate ensures the junk path is never taken.

**How to remove:**
- Resolve the opaque predicate (same technique as above) — force the branch to the real successor.
- Dead block elimination handles the junk blocks automatically.

#### Mixed Boolean-Arithmetic (MBA)

Simple operations disguised as complex boolean-arithmetic expressions.

**Common identities:**
- `(x ^ y) + 2*(x & y)` = `x + y`
- `(x | y) - (x & ~y)` = `y`
- `~(~x & ~y)` = `x | y`
- `(x & 0xFF) | (x & ~0xFF)` = `x`
- `(x | y) + (x & y)` = `x + y` (alternative form)

**How to remove:**
- When both operands are constants: compute the result, replace with a mov/constant.
- When operands are symbolic: pattern-match known identities and replace with the simplified form.
- Work bottom-up (simplify inner expressions first).
- Apply `(1 << (size * 8)) - 1` bitmask when constant-folding to handle unsigned overflow.
- Multiple passes may be needed — MBA expressions can be nested.

#### Instruction Substitution

Simple operations replaced with equivalent complex sequences. Common in OLLVM.

**Common patterns (match and reverse):**
- `a + b` replaced by: `a - (-b)`, `(a ^ b) + 2*(a & b)`, `(a | b) + (a & b)`
- `a - b` replaced by: `a + (-b)`, `(a ^ b) - 2*(~a & b)`
- `a ^ b` replaced by: `(a | b) - (a & b)`, `(~a & b) | (a & ~b)`
- `a & b` replaced by: `(a | b) - (a ^ b)`, `~(~a | ~b)`
- `a | b` replaced by: `(a & b) + (a ^ b)`, `~(~a & ~b)`

**How to remove:** Pattern-match the complex sequence and replace with the original simple operation. Multiple passes may be needed (substitutions can be chained).

#### Junk Code / Dead Stores

Instructions computing values never used.

- Identify via def-use chains: if a variable is defined but has no uses before its next definition, the definition is dead.
- A `mov` to a variable that is immediately overwritten is a dead store — NOP it.
- After CFF removal, many state variable assignments become dead — clean them up.

#### Anti-Disassembly

Junk bytes inserted to confuse linear disassembly.

**Common patterns:**
- Junk bytes after unconditional jumps: `jmp +2; db 0xE8` (fake call prefix).
- Overlapping instructions exploiting x86 variable-length encoding.

**How to fix:** NOP the junk bytes at the byte level, or redefine function boundaries. Check disassembly first — the disassembler may already handle some patterns.

### 3. VM Boundary (if detected)

Not reversible with IL-level tools.
1. Identify: VM entry point, bytecode buffer, handler table address, handler count.
2. Document all addresses and the dispatch mechanism.
3. Focus deobfuscation on non-virtualized code paths.
4. Recommend specialized VM lifting tooling (e.g., Miasm, Triton).

### 4. Post-Deobfuscation

- Redecompile all modified functions.
- Code should now be readable. If not, check for more obfuscation layers.
- Apply all annotations (renamed functions, decrypted string comments, recovered types).
- Proceed with normal analysis.

## Critical Rules

- **Read the IL before acting.** One read call beats 20 guesses.
- **String decryption ALWAYS comes first** (unless user specifies otherwise).
- **After each modification, redecompile to verify improvement.**
- **Iterative approach:** complex obfuscation needs multiple passes — install optimizer/transform, redecompile, check, refine.
- **If IL-level approach fails, fall back to byte-level patching.**
- **Never draw conclusions from obfuscated code.**
- **Check the host-specific reference files** for tool-specific rules (maturity guards, operand comparison, dirty marking).
