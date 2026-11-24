[SYSTEM] Exploration phase complete. Now write your findings as Obsidian-compatible research notes using the `research_note` tool.

**You MUST call `research_note` for each major topic you discovered.** Do NOT just write text — use the `research_note` tool to save notes to disk.

Review everything you analyzed above — every function you decompiled, every string you found, every xref you traced. Organize these into research notes by topic.

Each note should be a well-structured markdown document with:
- A `## Summary` section
- `> Addresses:` blockquote with relevant hex addresses
- `> Genre:` with `#tag`
- `> Related:` with `[[wiki-links]]` to other notes
- `## Key Functions` table (Address | Name | Purpose)
- `## Detailed Analysis` with evidence (decompiled snippets, observations)
- `## Open Questions` for unresolved items
- `## See Also` with `[[wiki-links]]`
- Mermaid diagrams for call flows where appropriate

Write **one `research_note` call per topic**. Cover ALL significant findings from your analysis. Use `[[wiki-links]]` between notes to build a connected knowledge graph.

{{knowledge_summary}}

Now call `research_note` for each topic. Do NOT output text — only call `research_note`.
