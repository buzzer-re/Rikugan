---
name: vuln-audit
description: Security audit — buffer overflows, format strings, integer issues, memory safety
---
Task: Security Vulnerability Audit. You are auditing a binary for exploitable vulnerabilities.

## Approach

Systematic, evidence-based. Every finding needs: location (address), root cause, impact assessment, and proof from the decompiled code.

## Phase 1: Attack Surface Mapping

1. `list_imports` — identify dangerous APIs:
   - **Memory**: memcpy, memmove, strcpy, strncpy, sprintf, vsprintf, gets
   - **Format strings**: printf, fprintf, syslog, snprintf with user-controlled format
   - **Heap**: malloc, free, realloc (use-after-free, double-free)
   - **File I/O**: fopen, CreateFile, read, write (path traversal)
   - **Network**: recv, recvfrom, WSARecv (remote input)
   - **Command**: system, popen, execve, ShellExecute (command injection)
2. `list_exports` — identify entry points accessible to attackers
3. `search_strings` — look for format strings, SQL patterns, command templates

## Phase 2: Input Tracing

For each dangerous API found:
1. `xrefs_to` on the import — find all call sites
2. `decompile_function` on each caller
3. Trace backwards: where does the buffer/size/format argument come from?
4. Is it user-controlled? (network input, file input, IPC, environment)
5. Are there bounds checks between input and dangerous API?

## Phase 3: Vulnerability Classes

**Buffer Overflow (Stack)**
- Fixed-size stack buffer + unbounded copy (strcpy, sprintf, gets)
- Size parameter larger than destination buffer
- Off-by-one in loop bounds writing to stack buffer

**Buffer Overflow (Heap)**
- malloc(user_size) without upper bound check
- memcpy into heap buffer with unchecked length
- Integer overflow in size calculation → small allocation, large copy

**Format String**
- printf(user_input) without format specifier
- syslog, fprintf with attacker-controlled first argument

**Integer Overflow/Underflow**
- Arithmetic on user-controlled sizes before allocation
- Signed/unsigned comparison mismatches in bounds checks
- Multiplication overflow in array index calculations

**Use-After-Free**
- free() followed by continued use of the pointer
- Dangling pointers in linked structures after partial cleanup
- Race conditions in multi-threaded free/use paths

**Command Injection**
- system() / popen() with string concatenation from user input
- ShellExecute with user-controlled arguments

**Type Confusion**
- Cast between incompatible struct types
- Virtual function table corruption paths
- Union member access after wrong variant initialization

## Phase 4: Report

For each finding:
```
[SEVERITY] Vulnerability Type at 0xADDRESS
Function: function_name
Root cause: <description>
Input path: <how attacker-controlled data reaches the vulnerable point>
Impact: <what an attacker can achieve>
Evidence: <relevant decompiled code snippet>
```

Severity levels: CRITICAL (remote code execution), HIGH (local code execution, info leak), MEDIUM (DoS, limited info leak), LOW (theoretical, requires unlikely conditions).
