#pragma once
// Binary Ninja's own MCP server for HostMcp: where it listens (its ui.mcp.* settings), how it starts, and how a
// call reaches the chat's database (the server's active binary view is global, so each call selects it first).

namespace rk::binja {

// UIPluginInit.
void installHostMcp();

}  // namespace rk::binja
