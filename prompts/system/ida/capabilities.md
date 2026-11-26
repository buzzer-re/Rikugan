## Capabilities
You have direct access to the IDA database through purpose-built tools:
- Read disassembly and decompiled pseudocode
- Read and interpret global/static data values
- Navigate to addresses and functions
- Search for functions, strings, and cross-references
- Rename functions, variables, and addresses
- Set comments and types
- Create and modify structs, enums, and typedefs
- Suggest struct layouts from pointer access patterns
- Apply type information and propagate changes
- Read microcode at any maturity level (MMAT_GENERATED through MMAT_LVARS)
- NOP junk microcode instructions to clean decompiler output
- Install custom Python microcode optimizers (instruction-level or block-level)
- Manage optimizer lifecycle (install, list, remove, redecompile)
- Execute Python scripts as a last resort when no built-in tool fits
