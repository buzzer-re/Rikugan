### BinaryView — Full Property Reference

| Property | Type | Description |
|----------|------|-------------|
| `bv.functions` | `list[Function]` | All functions |
| `bv.data_vars` | `dict[int, DataVariable]` | All data variables (addr → DataVariable) |
| `bv.symbols` | `dict[str, list[Symbol]]` | All symbols (name → list) |
| `bv.types` | `dict[str, Type]` | All defined types |
| `bv.sections` | `dict[str, Section]` | All sections |
| `bv.segments` | `list[Segment]` | All segments |
| `bv.arch` | `Architecture` | CPU architecture |
| `bv.platform` | `Platform` | Execution platform |
| `bv.entry_point` | `int` | Entry point address |
| `bv.start` / `bv.end` | `int` | Address range |
| `bv.hlil_instructions` | `Iterator` | ALL HLIL instructions across entire binary |

### BinaryView — Key Methods

```
bv.read(addr, length) -> bytes
bv.read8/16/32/64(addr) -> int
bv.read_pointer(addr) -> int
bv.write(addr, data) -> int
bv.get_function_at(addr) -> Function|None
bv.get_functions_containing(addr) -> list[Function]
bv.get_functions_by_name(name) -> list[Function]
bv.add_user_function(addr)
bv.remove_user_function(func)
bv.get_strings() -> list[StringReference]
bv.get_strings(start, length) -> list[StringReference]
bv.get_string_at(addr) -> StringReference|None
bv.find_next_data(start, pattern) -> int|None
bv.get_code_refs(addr) -> list[ReferenceSource]
bv.get_code_refs_from(addr) -> list[ReferenceSource]
bv.get_data_refs(addr) -> list[int]
bv.get_data_refs_from(addr) -> list[int]
bv.get_callers(addr) -> list
bv.get_callees(addr) -> list
bv.get_code_refs_for_type(name) -> list
bv.get_data_refs_for_type(name) -> list
bv.define_user_type(name, Type)
bv.define_user_data_var(addr, Type|str)
bv.undefine_user_data_var(addr)
bv.get_data_var_at(addr) -> DataVariable|None
bv.define_user_symbol(Symbol)
bv.undefine_user_symbol(Symbol)
bv.get_symbol_at(addr) -> Symbol|None
bv.get_symbols_by_name(name) -> list[Symbol]
bv.get_symbols_of_type(SymbolType) -> list[Symbol]
bv.get_segment_at(addr) -> Segment|None
bv.get_sections_at(addr) -> list[Section]
bv.get_section_by_name(name) -> Section|None
bv.parse_type_string(type_str) -> (Type, str)
bv.update_analysis_and_wait()
bv.abort_analysis()
```

### Function — Properties

| Property | Type | Description |
|----------|------|-------------|
| `func.name` | `str` | Name (settable) |
| `func.address` / `func.start` | `int` | Start address |
| `func.total_bytes` | `int` | Size in bytes |
| `func.type` | `FunctionType` | Function signature (settable) |
| `func.return_type` | `Type` | Return type |
| `func.parameter_vars` | `list[Variable]` | Parameters |
| `func.calling_convention` | `CallingConvention` | CC |
| `func.basic_blocks` | `list[BasicBlock]` | Blocks |
| `func.llil` | `LowLevelILFunction` | Low-level IL |
| `func.mlil` | `MediumLevelILFunction` | Medium-level IL |
| `func.hlil` | `HighLevelILFunction` | High-level IL |
| `func.llil_if_available` | `LLIL\|None` | Non-blocking |
| `func.mlil_if_available` | `MLIL\|None` | Non-blocking |
| `func.hlil_if_available` | `HLIL\|None` | Non-blocking |
| `func.callers` | `list[Function]` | Functions calling this |
| `func.callees` | `list[Function]` | Functions called by this |
| `func.caller_sites` | `list[ReferenceSource]` | Call site locations |
| `func.call_sites` | `list` | Outgoing call sites |
| `func.vars` | `list[Variable]` | All variables |
| `func.comments` | `dict[int, str]` | Address→comment map |

### Function — Key Methods

```
func.set_comment_at(addr, text)
func.get_comment_at(addr) -> str
func.get_llil_at(addr) -> LLILInstruction
func.create_user_var(var, type, name)
func.create_user_stack_var(offset, type, name)
func.set_user_instr_highlight(addr, HighlightStandardColor)
func.add_tag(tag_type, data, addr=None)
func.get_hlil_var_refs(var) -> list
```

### BasicBlock

| Property | Type | Description |
|----------|------|-------------|
| `bb.start` / `bb.end` | `int` | Address range |
| `bb.length` | `int` | Size |
| `bb.instruction_count` | `int` | Instruction count |
| `bb.incoming_edges` | `list[BasicBlockEdge]` | Predecessors |
| `bb.outgoing_edges` | `list[BasicBlockEdge]` | Successors |
| `bb.dominators` | `set[BasicBlock]` | Dominating blocks |
| `bb.immediate_dominator` | `BasicBlock` | Immediate dominator |
| `bb.post_dominators` | `set[BasicBlock]` | Post-dominating blocks |

`BasicBlockEdge`: `.type` (TrueBranch/FalseBranch/UnconditionalBranch), `.source`, `.target`, `.back_edge`

### Intermediate Languages (BNIL)

**Hierarchy** (low → high): Lifted IL → LLIL → LLIL SSA → MLIL → MLIL SSA → HLIL → HLIL SSA

**HLIL** is best for analysis — pseudocode-like with control flow structures (if/while/for/switch), type info, and dead code elimination.

**MLIL** is best for data flow — variables replace registers, types associated.

**LLIL** is closest to assembly — operates on registers and flags.

**Iteration patterns:**
```python
# Per-function HLIL
for inst in func.hlil.instructions:
    print(f"{hex(inst.address)}: {inst}")

# Per-block
for bb in func.hlil:
    for inst in bb:
        print(inst)

# Entire binary
for inst in bv.hlil_instructions:
    print(inst)

# MLIL
for inst in func.mlil.instructions:
    print(inst)
```

**Instruction type checking (commonil):**
```python
from binaryninja.commonil import Call, Comparison, Store, Load, Localcall

if isinstance(inst, Call):
    print(f"Call to {inst.dest} with {len(inst.params)} params")
elif isinstance(inst, Comparison):
    print(f"Comparing {inst.left} and {inst.right}")
elif isinstance(inst, Store):
    print(f"Store to {inst.dest}")
elif isinstance(inst, Load):
    print(f"Load from {inst.src}")
```

**Key HLIL instruction properties:**
- `inst.operation` — HighLevelILOperation enum
- `inst.address` — original address
- `inst.size` — operand size in bytes
- `inst.detailed_operands` — list of (name, value, type) tuples
- `inst.vars_written` — variables modified
- `inst.src` / `inst.dest` — source/destination
- `inst.left` / `inst.right` — binary operation operands

**Common operations:** `HLIL_CALL`, `HLIL_ASSIGN`, `HLIL_VAR`, `HLIL_VAR_INIT`, `HLIL_IF`, `HLIL_WHILE`, `HLIL_FOR`, `HLIL_SWITCH`, `HLIL_RET`, `HLIL_DEREF`, `HLIL_DEREF_FIELD`, `HLIL_ARRAY_INDEX`, `HLIL_ADD`, `HLIL_SUB`, `HLIL_CMP_E`, `HLIL_CMP_NE`

**SSA variable tracking:**
```python
ssa = func.hlil.ssa_form
defn = ssa.get_ssa_var_definition(ssa_var)
uses = ssa.get_ssa_var_uses(ssa_var)
```

**Visitor API:**
```python
def visitor(name, instr, type_name, parent):
    if isinstance(instr, Call):
        print(f"Found call: {instr}")
    return True  # continue visiting
func.hlil.visit(visitor)
```

**Level navigation:**
```python
llil_inst.hlil        # LLIL → HLIL
hlil_inst.mlils       # HLIL → contributing MLIL instructions
hlil_inst.llils       # HLIL → contributing LLIL instructions
```

### Type System — Complete Reference

**Immutable creation:**
```python
Type.int(4)                             # int32
Type.int(8, False)                      # uint64
Type.char()                             # char
Type.bool()                             # bool
Type.float(4)                           # float
Type.float(8)                           # double
Type.void()                             # void
Type.pointer(bv.arch, inner_type)       # pointer
Type.array(element_type, count)         # array
Type.function(ret_type, [(name, type)]) # function type
Type.structure(members=[(type, name)])  # struct (one-shot)
Type.enumeration(members=[(name, val)]) # enum (one-shot)
Type.named_type_from_registered_type(bv, "Name")  # reference to defined type
```

**Builder pattern (complex types):**
```python
from binaryninja import StructureBuilder, EnumerationBuilder

s = StructureBuilder.create()
s.append(Type.int(4), "x")
s.append(Type.pointer(bv.arch, Type.char()), "name")
s.append(Type.array(Type.int(1), 64), "buf")
bv.define_user_type("MyStruct", Type.structure_type(s))

e = EnumerationBuilder.create()
e.append("NONE", 0)
e.append("READ", 1)
e.append("WRITE", 2)
bv.define_user_type("MyEnum", Type.enumeration_type(bv.arch, e))
```

**Applying types:**
```python
# To data variables
bv.define_user_data_var(addr, Type.int(4))
bv.define_user_data_var(addr, "char*")  # string shorthand
ntr = Type.named_type_from_registered_type(bv, "MyStruct")
bv.define_user_data_var(addr, ntr)

# To functions
func.type = Type.function(Type.void(), [("buf", Type.pointer(bv.arch, Type.char()))])

# To variables
func.parameter_vars[0].type = Type.pointer(bv.arch, Type.char())

# Parse from C string
t, name = bv.parse_type_string("uint64_t*")
```

### Symbols

```python
from binaryninja import Symbol, SymbolType

# Types: FunctionSymbol, DataSymbol, ImportAddressSymbol,
#        ImportedFunctionSymbol, ImportedDataSymbol, ExternalSymbol

# Create and register
sym = Symbol(SymbolType.DataSymbol, addr, "g_config")
bv.define_user_symbol(sym)

# Rename function (simplest)
func.name = "NewName"

# Lookup
bv.get_symbol_at(addr)
bv.get_symbols_by_name("main")
bv.get_symbols_of_type(SymbolType.FunctionSymbol)
```

### UI Interaction

```python
from binaryninja.interaction import *
from binaryninja.enums import MessageBoxButtonSet, MessageBoxIcon

# Message box
result = show_message_box("Title", "Text",
    MessageBoxButtonSet.YesNoCancelButtonSet,
    MessageBoxIcon.QuestionIcon)

# Input
text = get_text_line_input("Prompt:", "Title")
num = get_int_input("Count:", "Title")
addr = get_address_input("Address:", "Title", bv, current_addr)
idx = get_choice_input("Pick:", "Title", ["A", "B", "C"])

# Reports (best for showing results)
show_plain_text_report("Title", text)
show_markdown_report("Title", "# Heading\n- item")
show_html_report("Title", "<h1>HTML</h1>")

# Multi-field form
fields = [
    TextLineField("Name:"),
    IntegerField("Count:"),
    ChoiceField("Type:", ["A", "B"]),
    CheckboxField("Enable:"),
]
if get_form_input(fields, "Config"):
    name = fields[0].result
```

### Database & Snapshots

```python
db = bv.file.database
db.write_global("key", "value")       # store string
val = db.read_global("key")           # retrieve string
db.write_global_data("key", buf)      # store binary
buf = db.read_global_data("key")      # retrieve binary

for snap in db.snapshots:
    print(f"{snap.id}: {snap.name}")
```

### Threading

```python
from binaryninja.mainthread import (
    execute_on_main_thread,
    execute_on_main_thread_and_wait,
    worker_enqueue,
    is_main_thread,
)

# UI operations must run on main thread
execute_on_main_thread(lambda: show_message_box(...))

# Heavy work on background thread
worker_enqueue(lambda: process_all_functions(bv))
```

### Transforms

```python
from binaryninja import Transform

sha = Transform["SHA256"]
result = sha.encode(b"data")

xor = Transform["XOR"]
dec = xor.encode(encrypted, {"key": key_bytes})

hex_t = Transform["RawHex"]
hex_str = hex_t.encode(data)
```

### BinaryReader / BinaryWriter

```python
from binaryninja import BinaryReader, BinaryWriter

reader = BinaryReader(bv)
reader.seek(addr)
val = reader.read32()
buf = reader.read(length)

writer = BinaryWriter(bv)
writer.seek(addr)
writer.write32(value)
writer.write(data)
```

### Notifications (Monitoring Changes)

```python
from binaryninja import BinaryDataNotification

class MyNotifier(BinaryDataNotification):
    def function_added(self, view, func):
        print(f"New function: {func.name}")
    def symbol_updated(self, view, sym):
        print(f"Symbol changed: {sym.name}")

notifier = MyNotifier()
bv.register_notification(notifier)
# bv.unregister_notification(notifier) when done
```

### Highlight Colors

```python
from binaryninja.enums import HighlightStandardColor

func.set_user_instr_highlight(addr, HighlightStandardColor.BlueHighlightColor)
# Colors: RedHighlightColor, BlueHighlightColor, GreenHighlightColor,
#         CyanHighlightColor, MagentaHighlightColor, YellowHighlightColor,
#         OrangeHighlightColor, WhiteHighlightColor, NoHighlightColor
```

### Tags

```python
# Function-level tag
func.add_tag("Important", "Main entry")

# Address-level tag
func.add_tag("Bug", "Buffer overflow here", addr)

# Get tags
tags = func.get_function_tags()
tags = func.get_address_tags_at(addr)
```

### IL Modification API

**Expression replacement:**
```python
# replace_expr(old_expr, new_expr) — swap an IL expression in-place
il_func.replace_expr(old_expr, new_expr)

# Create replacement expressions
il_func.const(size, value)           # constant value
il_func.nop()                        # NOP expression
il_func.goto(label)                  # unconditional goto
il_func.if_expr(cond, true_label, false_label)  # conditional
```

**Copy transformation (rebuild function):**
```python
il_func.prepare_to_copy_function(src_func)
il_func.prepare_to_copy_block(block)
il_func.append(instr.copy_to(il_func))
il_func.copy_to(block)
```

**Finalization (required after any modification):**
```python
il_func.finalize()               # finalize IL changes
il_func.generate_ssa_form()      # regenerate SSA
```

**Workflow and Activity API:**
```python
from binaryninja import Workflow, Activity

# Clone the default analysis workflow
workflow = Workflow("core.function.metaAnalysis").clone("CustomName")

# Create an activity wrapping a transform function
# Transform signature: def transform(analysis_context): ...
activity = Activity("activity.name", action=transform_fn)

# Register and insert into pipeline
workflow.register_activity(activity)
workflow.insert_after("core.function.generateMediumLevelIL", "activity.name")
# or: workflow.insert_before(...)
workflow.register()
```

**AnalysisContext (available in workflow activities):**
```python
def my_transform(analysis_context):
    func = analysis_context.function
    llil = analysis_context.llil       # LowLevelILFunction
    mlil = analysis_context.mlil       # MediumLevelILFunction (if available)
    # Modify IL, then:
    llil.finalize()
    llil.generate_ssa_form()
```

**Pipeline stages:**
- `core.function.translateTailCalls`
- `core.function.generateLowLevelIL`
- `core.function.generateMediumLevelIL`
- `core.function.generateHighLevelIL`
