### Core Modules

| Module | Purpose |
|--------|---------|
| `idautils` | Convenience iterators: Functions, Segments, Heads, Names, Strings, XrefsTo/From, CodeRefsTo/From, DataRefsTo/From |
| `idc` | IDC-compatible wrappers: get_func_name, set_name, add_func, get_wide_byte, create_strlit |
| `idaapi` | Legacy monolithic re-export; get_screen_ea, get_full_flags, FlowChart, BADADDR |
| `ida_funcs` | func_t, add_func, del_func, get_func, get_next_func, get_func_name |
| `ida_bytes` | get_bytes, patch_bytes, get_byte/word/dword/qword, get_strlit_contents, create_data, get_full_flags, is_code, is_data |
| `ida_name` | get_name, set_name, demangle_name, get_name_ea, SN_CHECK/SN_FORCE |
| `ida_segment` | segment_t, getseg, get_segm_by_name, get_segm_name |
| `ida_typeinf` | tinfo_t, udt_type_data_t, udm_t, edm_t, enum_type_data_t, func_type_data_t, apply_tinfo, apply_cdecl, parse_decl, get_idati |
| `ida_xref` | xrefblk_t, add_cref, add_dref, XREF_ALL, XREF_FAR |
| `ida_hexrays` | decompile, cfunc_t, citem_t, cexpr_t, cinsn_t, ctree_visitor_t, mba_t, mblock_t, minsn_t, mop_t, lvar_t |
| `ida_kernwin` | ask_str, ask_yn, warning, msg, jumpto, get_screen_ea, register_action, action_handler_t, UI_Hooks |
| `ida_lines` | generate_disasm_line, tag_remove |
| `ida_search` | find_binary, find_text, find_code, find_data, SEARCH_DOWN/UP/NEXT/REGEX |
| `ida_nalt` | get_imagebase, get_root_filename, get_input_file_path, STRTYPE_C |
| `ida_ida` | inf_get_procname, inf_is_64bit, inf_is_32bit_exactly, inf_get_min_ea, inf_get_max_ea |
| `ida_entry` | get_entry_qty, get_entry, get_entry_ordinal, get_entry_name |
| `ida_frame` | get_func_frame (IDA 9), define_stkvar, build_stkvar_xrefs |
| `ida_auto` | auto_wait, plan_and_wait, auto_mark_range |
| `ida_gdl` | FlowChart, BasicBlock (start_ea, end_ea, succs, preds) |
| `ida_netnode` | Persistent key-value storage: supval, altval, hashval, blob |

### func_t Properties

| Property | Type | Description |
|----------|------|-------------|
| `start_ea` | `ea_t` | Function start address |
| `end_ea` | `ea_t` | Function end address |
| `flags` | `int` | FUNC_LIB, FUNC_STATIC, FUNC_FRAME, FUNC_THUNK |
| `frame` | `tid_t` | Frame structure ID |
| `frsize` | `int` | Local variables area size |
| `argsize` | `int` | Arguments area size |

### Hex-Rays Decompiler — CTree

**cfunc_t** (decompiled function container):
- `entry_ea` — function entry address
- `body` — function body as cinsn_t (cit_block)
- `lvars` — local variables vector
- `mba` — underlying microcode (mba_t)
- `get_pseudocode()` — simpleline_t vector
- `get_lvars()` — local variable list

**citem_t** (abstract base):
- `ea` — associated address
- `op` — node type (ctype_t)
- `is_expr()` — True if expression

**cexpr_t** (expressions, inherits citem_t):
- `type` — tinfo_t (result type)
- `x`, `y`, `z` — sub-expressions
- `n` — cnumber_t (when op == cot_num)
- `v` — var_ref_t (when op == cot_var)
- `obj_ea` — ea_t (when op == cot_obj)
- `string` — string literal (when op == cot_str)
- `a` — carglist_t (when op == cot_call)
- `m` — member offset (when op == cot_memptr/cot_memref)

**cinsn_t** (statements, inherits citem_t):
- `cblock` — cblock_t (when op == cit_block)
- `cif` — cif_t with condition + then/else (when op == cit_if)
- `cfor` — cfor_t (when op == cit_for)
- `cwhile` — cwhile_t (when op == cit_while)
- `cswitch` — cswitch_t (when op == cit_switch)
- `creturn` — creturn_t (when op == cit_return)

**Key expression opcodes (cot_*):**

| Op | Meaning | Key fields |
|----|---------|------------|
| `cot_call` | x(...) | x=callee, a=args |
| `cot_num` | number | n._value |
| `cot_var` | local variable | v.idx |
| `cot_obj` | global/import | obj_ea |
| `cot_str` | string literal | string |
| `cot_asg` | x = y | x, y |
| `cot_add/sub/mul` | arithmetic | x, y |
| `cot_eq/ne/slt/ult` | comparison | x, y |
| `cot_band/bor/xor` | bitwise | x, y |
| `cot_lor/land` | logical | x, y |
| `cot_ptr` | *x (deref) | x |
| `cot_ref` | &x (address-of) | x |
| `cot_idx` | x[y] (index) | x, y |
| `cot_memptr` | x->m | x, m |
| `cot_memref` | x.m | x, m |
| `cot_cast` | (type)x | x |
| `cot_tern` | x ? y : z | x, y, z |
| `cot_helper` | helper call | helper |

**Key statement opcodes (cit_*):**
`cit_block`, `cit_expr`, `cit_if`, `cit_for`, `cit_while`, `cit_do`, `cit_switch`, `cit_return`, `cit_goto`, `cit_asm`, `cit_try` (IDA 9+), `cit_throw` (IDA 9+)

**ctree_visitor_t:**
```python
class MyVisitor(ida_hexrays.ctree_visitor_t):
    def __init__(self):
        super().__init__(ida_hexrays.CV_FAST)
        # CV_FAST = no parent tracking
        # CV_PARENTS = track parents (slower)
        # CV_POST = post-order
    def visit_insn(self, insn):  # cinsn_t
        return 0  # 0=continue
    def visit_expr(self, expr):  # cexpr_t
        return 0
visitor = MyVisitor()
visitor.apply_to(cfunc.body, None)
```

**lvar_t** (local variable):
- `name` — variable name
- `type()` — tinfo_t
- `is_arg_var` — True if function argument
- `is_stk_var()` — True if stack variable
- `is_reg_var()` — True if register variable

### Microcode API

**Maturity levels (mba_maturity_t):**

| Level | Name | Description |
|-------|------|-------------|
| 1 | MMAT_GENERATED | Direct asm translation |
| 2 | MMAT_PREOPTIMIZED | Dead code + propagation |
| 3 | MMAT_LOCOPT | Local optimization, CFG ready |
| 4 | MMAT_CALLS | Call ABI analysis |
| 5 | MMAT_GLBOPT1 | Block merging |
| 6 | MMAT_GLBOPT2 | Further global opt |
| 7 | MMAT_GLBOPT3 | Final global opt |
| 8 | MMAT_LVARS | SSA, register→variable |

**mba_t** (microcode block array):
- `qty` — number of blocks
- `get_mblock(n)` — get mblock_t by index
- `entry_idx` / `exit_idx` — entry/exit block indices
- `maturity` — current level

**mblock_t** (basic block):
- `head` / `tail` — first/last minsn_t
- `serial` — block number
- `npred()` / `nsucc()` — predecessor/successor count
- `pred(n)` / `succ(n)` — get pred/succ block number
- `insert_into_block(insn, after)` — add instruction
- `remove_from_block(insn)` — remove instruction

**minsn_t** (microcode instruction):
- `opcode` — mcode_t (m_mov, m_ldc, m_stx, m_ldx, m_add, m_sub, m_call, m_jcnd, m_goto, m_ret, etc.)
- `l` / `r` / `d` — left, right, destination (mop_t)
- `next` / `prev` — instruction chain
- `ea` — original address

**Key microcode opcodes:**

| Category | Opcodes |
|----------|---------|
| Memory | m_stx, m_ldx, m_mov, m_ldc |
| Arithmetic | m_add, m_sub, m_mul, m_udiv, m_sdiv, m_umod, m_smod |
| Bitwise | m_and, m_or, m_xor, m_shl, m_shr, m_sar, m_bnot |
| Comparison | m_setz, m_setnz, m_setb, m_seta, m_setl, m_setg |
| Control | m_jcnd, m_jnz, m_jtbl, m_goto, m_call, m_ret |

**mop_t** (operand):
- `t` — type (mop_r=reg, mop_n=number, mop_d=nested, mop_S=stack, mop_v=global, mop_l=local, mop_b=block, mop_f=call)
- `r` — register (when t=mop_r)
- `nnn` — mnumber_t with `.value` (when t=mop_n)
- `d` — nested minsn_t (when t=mop_d)
- `g` — global ea_t (when t=mop_v)
- `size` — operand byte size

**Microcode traversal:**
```python
cfunc = ida_hexrays.decompile(ea)
mba = cfunc.mba
for i in range(mba.qty):
    blk = mba.get_mblock(i)
    insn = blk.head
    while insn:
        if insn.opcode == ida_hexrays.m_call:
            print(f"Call at {hex(insn.ea)}")
        if insn == blk.tail:
            break
        insn = insn.next
```

**Custom optimizers:**
```python
class MyOpt(ida_hexrays.optinsn_t):
    def func(self, blk, insn, optflags):
        # Return number of changes made
        return 0
opt = MyOpt()
opt.install()   # activate
opt.remove()    # deactivate
```

**Generating microcode at specific maturity:**
```python
func = ida_funcs.get_func(ea)
mbr = ida_hexrays.mba_ranges_t(func)
hf = ida_hexrays.hexrays_failure_t()
ida_hexrays.mark_cfunc_dirty(func.start_ea)
mba = ida_hexrays.gen_microcode(mbr, hf, None,
    ida_hexrays.DECOMP_NO_WAIT, ida_hexrays.MMAT_GLBOPT1)
```

### Type System (ida_typeinf) — Complete Reference

**tinfo_t construction:**
```python
tif = ida_typeinf.tinfo_t()
tif.parse("int *")                      # from C string
tif = ida_typeinf.tinfo_t("DWORD")     # from named type
tif.create_simple_type(ida_typeinf.BT_INT32)  # from constant
```

**tinfo_t queries:**
- `is_ptr()`, `is_array()`, `is_func()`, `is_struct()`, `is_union()`, `is_udt()`, `is_enum()`
- `is_int()`, `is_float()`, `is_bool()`, `is_signed()`, `is_unsigned()`
- `get_size()` — byte size
- `get_type_name()` — name if available
- `dstr()` — debug string
- `empty()` / `present()` — validity

**Creating structs (IDA 9+):**
```python
tif = ida_typeinf.tinfo_t()
udt = ida_typeinf.udt_type_data_t()
udm = ida_typeinf.udm_t()
udm.name = "field1"
udm.type = ida_typeinf.tinfo_t(ida_typeinf.BT_INT32)
udm.offset = 0  # bit offset
udt.push_back(udm)
tif.create_udt(udt, ida_typeinf.BTF_STRUCT)
tif.set_named_type(None, "MyStruct")
```

**Creating enums:**
```python
tif = ida_typeinf.tinfo_t()
edt = ida_typeinf.enum_type_data_t()
edm = ida_typeinf.edm_t()
edm.name = "VAL_A"; edm.value = 0
edt.push_back(edm)
tif.create_enum(edt)
tif.set_named_type(None, "MyEnum")
```

**Creating function types:**
```python
tif = ida_typeinf.tinfo_t()
ftd = ida_typeinf.func_type_data_t()
ftd.rettype = ida_typeinf.tinfo_t("int")
arg = ida_typeinf.funcarg_t()
arg.name = "param1"
arg.type = ida_typeinf.tinfo_t("char *")
ftd.push_back(arg)
tif.create_func(ftd)
```

**Applying types:**
```python
ida_typeinf.apply_tinfo(ea, tif, ida_typeinf.TINFO_DEFINITE)
ida_typeinf.apply_cdecl(None, ea, "int func(int a)")
```

**Iterating members (IDA 9+):**
```python
tif = ida_typeinf.tinfo_t("my_struct_t")
for udm in tif.iter_struct():
    print(f"  {udm.name}: {udm.type.dstr()} @ bit offset {udm.offset}")
```

**Type library:**
```python
til = ida_typeinf.get_idati()
for ordinal in range(1, ida_typeinf.get_ordinal_count(til) + 1):
    tif = ida_typeinf.tinfo_t()
    if tif.get_numbered_type(til, ordinal):
        print(f"#{ordinal}: {tif.dstr()}")
```

### Cross-References — Full API

**High-level (idautils):**
```python
# All refs TO address
for xref in idautils.XrefsTo(ea, ida_xref.XREF_ALL):
    # xref.frm, xref.to, xref.type, xref.iscode

# Code refs only
for ref in idautils.CodeRefsTo(ea, False):   # False=no flow
    pass
for ref in idautils.CodeRefsFrom(ea, False):
    pass

# Data refs only
for ref in idautils.DataRefsTo(ea):
    pass
for ref in idautils.DataRefsFrom(ea):
    pass
```

**Low-level (xrefblk_t):**
```python
xref = ida_xref.xrefblk_t()
ok = xref.first_to(ea, ida_xref.XREF_ALL)
while ok:
    print(f"from {hex(xref.frm)}, type={xref.type}")
    ok = xref.next_to()
```

**Xref types:** `fl_CF`/`fl_CN` (call far/near), `fl_JF`/`fl_JN` (jump far/near), `fl_F` (flow), `dr_R`/`dr_W` (data read/write), `dr_O` (offset)

**Add/delete xrefs:**
```python
ida_xref.add_cref(from_ea, to_ea, ida_xref.fl_CN)
ida_xref.add_dref(from_ea, to_ea, ida_xref.dr_R)
ida_xref.del_cref(from_ea, to_ea, 0)
ida_xref.del_dref(from_ea, to_ea)
```

### UI Interaction (ida_kernwin)

```python
# Messages
ida_kernwin.msg("Text\n")
ida_kernwin.warning("Warning popup")
ida_kernwin.info("Info popup")

# Input
name = ida_kernwin.ask_str("default", 0, "Prompt:")
yn = ida_kernwin.ask_yn(1, "Sure?")    # returns 1/0/-1

# Navigation
ea = ida_kernwin.get_screen_ea()
ida_kernwin.jumpto(ea)

# Progress
ida_kernwin.show_wait_box("Working...")
ida_kernwin.hide_wait_box()

# Custom action
class Handler(ida_kernwin.action_handler_t):
    def activate(self, ctx):
        return 1
    def update(self, ctx):
        return ida_kernwin.AST_ENABLE_ALWAYS

desc = ida_kernwin.action_desc_t("id", "Label", Handler(), "Ctrl+M", "Tooltip", -1)
ida_kernwin.register_action(desc)
ida_kernwin.attach_action_to_menu("Edit/", "id", ida_kernwin.SETMENU_APP)
```

### Hooks

**UI Hooks:**
```python
class MyUI(ida_kernwin.UI_Hooks):
    def screen_ea_changed(self, ea, prev_ea):
        return 0
    def populating_widget_popup(self, widget, popup):
        ida_kernwin.attach_action_to_popup(widget, popup, "my:action")
hooks = MyUI()
hooks.hook()
```

**Hexrays Hooks:**
```python
class MyHR(ida_hexrays.Hexrays_Hooks):
    def maturity(self, cfunc, maturity):
        return 0
    def func_printed(self, cfunc):
        return 0
hooks = MyHR()
hooks.hook()
```

**IDB Hooks:**
```python
class MyIDB(ida_idp.IDB_Hooks):
    def renamed(self, ea, new_name, is_local):
        return 0
    def auto_empty(self):
        return 0
hooks = MyIDB()
hooks.hook()
```

### Netnodes (Persistent Storage)

```python
node = ida_netnode.netnode("$my_data", 0, True)  # create=True
node.hashset("key", "value")           # store string
val = node.hashstr("key")              # retrieve
node.altset(0, 42)                     # store int
val = node.altval(0)                   # retrieve
node.setblob(data, 0, "B")            # store binary
data = node.getblob(0, "B")           # retrieve
node.kill()                            # delete
```

### Database Info (ida_ida / ida_nalt)

```python
base = ida_nalt.get_imagebase()
filename = ida_nalt.get_root_filename()
filepath = ida_nalt.get_input_file_path()
proc = ida_ida.inf_get_procname()
is_64 = ida_ida.inf_is_64bit()
is_32 = ida_ida.inf_is_32bit_exactly()
min_ea = ida_ida.inf_get_min_ea()
max_ea = ida_ida.inf_get_max_ea()
```

### Entry Points

```python
for i in range(ida_entry.get_entry_qty()):
    ordinal = ida_entry.get_entry_ordinal(i)
    ea = ida_entry.get_entry(ordinal)
    name = ida_entry.get_entry_name(ordinal)
    print(f"#{ordinal}: {name} @ {hex(ea)}")
```

### Stack Frames (IDA 9+)

```python
func = ida_funcs.get_func(ea)
tif = ida_typeinf.tinfo_t()
if ida_frame.get_func_frame(tif, func):
    for udm in tif.iter_struct():
        print(f"  {udm.name}: {udm.type.dstr()}")
```

### Disassembly Text

```python
line = ida_lines.generate_disasm_line(ea, ida_lines.GENDSM_FORCE_CODE)
clean = ida_lines.tag_remove(line)
```

### IDA 9.0 Breaking Changes

- `ida_struct` / `ida_enum` **removed** — use `ida_typeinf` (tinfo_t, udm_t, edm_t)
- `get_inf_structure()` **removed** — use `inf_get_*()` / `inf_is_*()` accessors
- `find_binary()` **deprecated** — use `ida_bytes.find_bytes()`
- Graph API: `abstract_graph_t` → `drawable_graph_t`, `mutable_graph_t` → `interactive_graph_t`
- Node/edge types moved from `ida_graph` to `ida_gdl`
- Struct/enum IDB events replaced by `local_types_changed`, `lt_udm_created`
