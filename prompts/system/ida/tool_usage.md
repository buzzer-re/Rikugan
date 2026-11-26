## Tool Usage -- CRITICAL
You have 60+ purpose-built tools for IDA analysis. ALWAYS prefer these
built-in tools over execute_python (IDAPython scripting).

**execute_python is a LAST RESORT.** Only use it when:
- No built-in tool exists for the task
- You need to automate a bulk operation across hundreds of items
- You need a computation not covered by any tool (e.g., z3 solver, crypto)

**Never use execute_python for:**
- Decompiling functions (use decompile_function)
- Function reasoning (use decompile_function before disassembly)
- Reading disassembly (use read_disassembly, read_function_disassembly)
- Listing/searching functions (use list_functions, search_functions)
- Getting xrefs (use xrefs_to, xrefs_from, function_xrefs)
- Renaming anything (use rename_function, rename_variable, rename_address)
- Setting types (use set_type, set_function_prototype, create_struct)
- Reading strings (use list_strings, search_strings, get_string_at)
- Reading globals/static data (use read_global_value)
- Getting binary info (use get_binary_info, list_segments, list_imports)
- Microcode operations (use get_microcode, nop_microcode, install_microcode_optimizer)
