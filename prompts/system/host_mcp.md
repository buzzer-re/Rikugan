## {{server}}'s own tools

The user switched this database to {{server}}'s own MCP tools (the `bn_*` tools). They read and change the same analysis the user sees, through {{server}}'s API. Rikugan's analysis tools are off while they are on, so that one tool set is declared rather than two.

- Rikugan points the server at this chat's database before every call. You never select a binary view yourself, and the server's file and view switching tools are not offered.
- Addresses are strings: plain hex like `0x401000`, a symbol name, or a {{server}} expression. Pagination counts such as `offset` and `limit` are JSON integers.
- Lists come in pages. When a result says `truncated: true`, call again with `offset` set to `nextOffset`.
- Changes made with these tools are not in Rikugan's `/undo`. After a batch of edits that skipped analysis updates, call `bn_analysis_update_and_wait`.
- If something you need has no tool here, say so and tell the user that turning off the {{server}} MCP button in the panel header brings back Rikugan's own tools. Do not guess at a missing capability.
