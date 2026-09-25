"""Global constants for Rikugan.

This module is data-only — no runtime detection or host probing.
For host capability flags see ``rikugan.core.host``.
"""

from __future__ import annotations

PLUGIN_NAME = "Rikugan"
PLUGIN_VERSION = "1.4.0"
PLUGIN_HOTKEY = "Ctrl+Shift+I"
PLUGIN_COMMENT = "Intelligent Reverse-engineering Integrated System"

CONFIG_DIR_NAME = "rikugan"
CONFIG_FILE_NAME = "config.json"
CHECKPOINTS_DIR_NAME = "checkpoints"

DEFAULT_CONTEXT_WINDOW = 200000

TOOL_RESULT_TRUNCATE_LEN = 8000

SYSTEM_PROMPT_VERSION = 1
CONFIG_SCHEMA_VERSION = 2
SESSION_SCHEMA_VERSION = 1

SKILLS_DIR_NAME = "skills"
MCP_CONFIG_FILE = "mcp.json"
# Deliberately not "mcp_". The Anthropic API reserves names matching that
# prefix exactly (one underscore) for its own MCP connector, which bills as a
# premium feature — a request declaring one is refused outright on a
# subscription token, with a message about extra usage that says nothing about
# tool names. Verified against the live API: "mcp_x" is refused, "mcp__x",
# "MCP_x", "mcp-x" and "bn_mcp_x" are all accepted.
MCP_TOOL_PREFIX = "mcptool_"
MCP_DEFAULT_TIMEOUT = 30.0
