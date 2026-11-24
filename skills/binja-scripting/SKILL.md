---
name: binja-scripting
description: Write and execute Binary Ninja Python scripts — full API reference included
---
Task: Help the user write Binary Ninja Python scripts. You have `execute_python` which runs code with `bv` (the current BinaryView), `binaryninja` module, and `current_address` pre-loaded.

Before starting, read skill://binja-scripting/references/api-reference.md with the read tool: it is the full API reference.

## Guidelines

- Use `print()` for all output — it's captured and returned to you.
- Prefer the Binary Ninja Python API over raw tool calls when the task requires iteration, filtering, or complex logic that a single tool can't express.
- Always handle `None` returns (e.g., `bv.get_function_at()` returns `None` if no function at that address).
- Use `bv.update_analysis_and_wait()` after bulk modifications (defining types, creating functions).
- For large outputs, summarize or paginate — don't dump thousands of lines.
- User types (`define_user_*`, `add_user_*`) persist to the database; auto types may be overwritten by analysis.
- When modifying the database (renaming, retyping, creating structs), prefer `_user_` methods.

## Environment

The `execute_python` tool provides:
- `bv` — the active `BinaryView`
- `binaryninja` — the full `binaryninja` module
- `binaryninjaui` — UI module (if available)
- `current_address` — cursor address (int, 0 if unavailable)
- Full Python stdlib (except subprocess/os.exec — blocked for safety)

## Quick Reference

### Reading Data
```python
data = bv.read(addr, length)       # raw bytes
val = bv.read32(addr)              # 32-bit int
ptr = bv.read_pointer(addr)        # pointer-sized int
strings = bv.get_strings()         # all StringReference objects
s = bv.get_string_at(addr)         # single string
```

### Functions
```python
func = bv.get_function_at(addr)           # exact start
funcs = bv.get_functions_containing(addr) # containing addr
funcs = bv.get_functions_by_name("main")  # by name (list)
bv.add_user_function(addr)                # create function
func.name = "NewName"                     # rename
func.type = Type.function(ret, params)    # retype
func.set_comment_at(addr, "note")         # comment
```

### IL Access
```python
# Three levels: llil < mlil < hlil (prefer hlil for analysis)
for inst in func.hlil.instructions:
    print(f"{hex(inst.address)}: {inst}")

# SSA form for data flow
defn = func.hlil.ssa_form.get_ssa_var_definition(ssa_var)
uses = func.hlil.ssa_form.get_ssa_var_uses(ssa_var)

# Navigate between levels
llil_inst = func.get_llil_at(addr)
hlil_from_llil = llil_inst.hlil
```

### Cross-References
```python
refs = bv.get_code_refs(addr)        # code refs TO addr
refs = bv.get_data_refs(addr)        # data refs TO addr
refs = bv.get_code_refs_from(addr)   # code refs FROM addr
callers = func.callers               # calling functions
callees = func.callees               # called functions
```

### Types
```python
from binaryninja import Type, StructureBuilder

# Primitives — ALWAYS use Type.* constructors, never raw strings
Type.int(4, True)                          # int32_t  (signed=True default)
Type.int(4, False)                         # uint32_t
Type.int(8, False)                         # uint64_t
Type.int(2, False)                         # uint16_t
Type.int(1, False)                         # uint8_t
Type.char()                                # char (signed byte)
Type.void()                                # void
Type.bool()                                # bool
Type.float(4)                              # float
Type.float(8)                              # double

# Pointers
Type.pointer(bv.arch, Type.char())         # char*
Type.pointer(bv.arch, Type.void())         # void*
Type.pointer(bv.arch, Type.int(4, False))  # uint32_t*
# char** (pointer-to-pointer)
Type.pointer(bv.arch, Type.pointer(bv.arch, Type.char()))

# Arrays — NEVER use string syntax "uint8_t[256]", it will fail
Type.array(Type.int(1, False), 256)        # uint8_t[256]
Type.array(Type.char(), 64)               # char[64]
Type.array(Type.int(4, False), 8)         # uint32_t[8]
```

### Struct Reconstruction (Critical Gotchas)
```python
from binaryninja import Type, StructureBuilder

# ✅ Correct — always use Type.* for field types, never strings
s = StructureBuilder.create()
s.append(Type.int(4, False), "a_type")      # uint32_t a_type
s.append(Type.int(8, False), "a_val")       # uint64_t a_val
s.append(Type.pointer(bv.arch, Type.char()), "name")  # char* name
s.append(Type.array(Type.int(1, False), 16), "buf")   # uint8_t buf[16]
bv.define_user_type("MyStruct", Type.structure_type(s))

# ❌ WRONG — string types silently fail (fields are dropped without error!)
s.append("uint32_t", "field")         # SILENT DROP
s.append("uint8_t[16]", "buf")        # PARSER ERROR or SILENT DROP

# Dependency order: define inner structs FIRST
s_inner = StructureBuilder.create()
s_inner.append(Type.int(4, False), "x")
s_inner.append(Type.int(4, False), "y")
bv.define_user_type("Entry", Type.structure_type(s_inner))

# Then reference with named_type_from_registered_type
s_outer = StructureBuilder.create()
entry_ref = Type.named_type_from_registered_type(bv, "Entry")
s_outer.append(entry_ref, "entry")               # Entry entry
s_outer.append(Type.array(entry_ref, 32), "tbl") # Entry tbl[32]
bv.define_user_type("Table", Type.structure_type(s_outer))

# ALWAYS validate after defining — never assume it worked
t = bv.get_type_by_name("MyStruct")
if t is None:
    print("ERROR: type not registered")
elif t.width == 0:
    print("ERROR: empty struct — field types were likely rejected")
else:
    print(f"OK: MyStruct, {t.width} bytes, {len(t.structure.members)} fields")
    for m in t.structure.members:
        print(f"  +{m.offset:#x}  {m.type}  {m.name}")

# Apply to data
ntr = Type.named_type_from_registered_type(bv, "MyStruct")
bv.define_user_data_var(addr, ntr)

# Enum
Type.enumeration(members=[("VAL_A", 0), ("VAL_B", 1)])

# Parse C — only works for types BN already knows (platform types)
# DO NOT use this for custom/typedef types; define them via Type.* API instead
t, name = bv.parse_type_string("uint64_t*")   # OK (platform type)
t, name = bv.parse_type_string("uint32_t")    # may fail on some platforms
```

### Resolving Platform Types by String (safe alternative)
```python
# If you must look up a type by C name, check it exists first
def resolve_type(bv, c_name: str):
    """Parse a C type name safely, return Type or None."""
    try:
        t, _ = bv.parse_type_string(c_name)
        return t
    except Exception:
        return None

# Mapping common C names to Type.* constructors (use these instead)
TYPE_MAP = {
    "uint8_t":  Type.int(1, False),
    "uint16_t": Type.int(2, False),
    "uint32_t": Type.int(4, False),
    "uint64_t": Type.int(8, False),
    "int8_t":   Type.int(1, True),
    "int16_t":  Type.int(2, True),
    "int32_t":  Type.int(4, True),
    "int64_t":  Type.int(8, True),
    "size_t":   Type.int(bv.arch.address_size, False),
    "uintptr_t":Type.int(bv.arch.address_size, False),
    "char":     Type.char(),
    "void":     Type.void(),
}
```

### Symbols
```python
from binaryninja import Symbol, SymbolType
sym = Symbol(SymbolType.DataSymbol, addr, "g_config")
bv.define_user_symbol(sym)
bv.get_symbol_at(addr)
bv.get_symbols_by_name("main")
```

### Segments & Sections
```python
for seg in bv.segments:
    print(f"{hex(seg.start)}-{hex(seg.end)}")
for name, sec in bv.sections.items():
    print(f"{name}: {hex(sec.start)}-{hex(sec.end)}")
bv.get_segment_at(addr)
bv.get_sections_at(addr)
```

### UI Interaction
```python
from binaryninja.interaction import (
    show_message_box, show_plain_text_report,
    show_markdown_report, show_html_report,
    get_text_line_input, get_int_input,
    get_choice_input, get_address_input,
)
# Reports are the best way to show formatted output
show_markdown_report("Title", "# Results\n- item 1\n- item 2")
show_plain_text_report("Title", large_text_output)
```

### Common Patterns
```python
# Find functions calling a specific import
target = bv.get_functions_by_name("CreateFileW")
if target:
    for caller in target[0].callers:
        print(f"{hex(caller.start)}: {caller.name}")

# Search for byte pattern
addr = bv.find_next_data(bv.start, b"\x48\x89\x5C\x24")

# Iterate all data variables
for addr, dv in bv.data_vars.items():
    print(f"{hex(addr)}: {dv.type} = {dv.name or '(unnamed)'}")

# Bulk rename with evidence
for func in bv.functions:
    if func.name.startswith("sub_"):
        hlil = func.hlil_if_available
        if hlil:
            for inst in hlil.instructions:
                # ... analyze and rename based on evidence
                pass

# Background task for long operations
from binaryninja import BackgroundTaskThread
class MyTask(BackgroundTaskThread):
    def __init__(self, bv):
        super().__init__("Processing...", can_cancel=True)
        self.bv = bv
    def run(self):
        for i, func in enumerate(self.bv.functions):
            if self.cancelled:
                break
            self.progress = f"{func.name} ({i}/{len(self.bv.functions)})"
```

## IL Modification

Binary Ninja supports IL-level modifications through its workflow system and `replace_expr` API.

### Expression Replacement
```python
# Replace an IL expression with a new one
# Works at LLIL and MLIL levels
il_func = analysis_context.llil  # or .mlil
expr = il_func[index]
new_expr = il_func.const(expr.size, value)
il_func.replace_expr(expr, new_expr)
il_func.finalize()
il_func.generate_ssa_form()
```

### Copy Transformation
```python
# Rebuild a function with structural changes (block reordering, removal)
il_func.prepare_to_copy_function(src_func)
for block in src_blocks:
    il_func.prepare_to_copy_block(block)
    for instr in block:
        il_func.append(instr.copy_to(il_func))
    il_func.copy_to(block)
il_func.finalize()
il_func.generate_ssa_form()
```

### Workflow Registration
```python
# Register a transform at a specific pipeline stage
from binaryninja import Workflow, Activity

workflow = Workflow("core.function.metaAnalysis").clone("MyWorkflow")
activity = Activity("myTransform", action=my_transform_func)
workflow.register_activity(activity)
workflow.insert_after("core.function.generateMediumLevelIL", "myTransform")
workflow.register()
```

### Pipeline Insertion Points
- Before `core.function.generateMediumLevelIL` — modify LLIL before MLIL generation
- After `core.function.generateMediumLevelIL` — modify MLIL after generation
- Must access IL through `AnalysisContext` during workflow activities
- Always call `finalize()` + `generate_ssa_form()` after modifications

### Label/GOTO for Control Flow
```python
# Create labels for control flow changes
label = il_func.get_label_for_address(il_func.arch, target_addr)
il_func.append(il_func.goto(label))
```

## Important Notes

- `func.hlil_if_available` returns `None` if HLIL hasn't been generated yet — safer than `func.hlil` which may block.
- `bv.functions` returns a snapshot list — safe to iterate while modifying.
- Use `Type.named_type_from_registered_type(bv, name)` to reference a type you've defined, not the raw Type object.
- `_user_` methods persist; `_auto_` methods may be overwritten by re-analysis.
- Process execution (subprocess, os.system, etc.) is blocked. Static analysis only.
