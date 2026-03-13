"""Network Reconstructor agent: prompt configuration and defaults."""

from __future__ import annotations

from .perks import build_perks_addendum

NETWORK_RECON_PROMPT = """\
You are a network protocol reverse engineer. Your task is to reconstruct
the network communication layer of this binary.

Workflow:
1. Find all socket/network API imports (connect, send, recv, WSA*,
   InternetOpen*, HttpSendRequest*, etc.)
2. Trace callers of each network API to find the communication functions
3. Identify:
   - Server addresses / domains (hardcoded or constructed)
   - Port numbers
   - Protocol type (HTTP, TCP raw, DNS, custom)
   - Encryption/encoding (XOR, RC4, AES, base64, custom)
   - C2 command structure (command IDs, dispatch tables)
   - Data exfiltration format
4. For each identified struct, declare it using declare_c_type
5. Output a structured summary with:
   - Network topology diagram (ASCII)
   - C struct definitions for all protocol messages
   - Command dispatch table
   - Encryption details"""

NETWORK_RECON_DEFAULT_PERKS: list[str] = [
    "import_mapping",
    "string_harvesting",
    "deep_decompilation",
]

NETWORK_RECON_MAX_TURNS: int = 30


def build_network_recon_addendum() -> str:
    """Build the full system addendum for a network recon subagent."""
    perks_text = build_perks_addendum(NETWORK_RECON_DEFAULT_PERKS)
    parts = [NETWORK_RECON_PROMPT]
    if perks_text:
        parts.append(perks_text)
    return "\n\n".join(parts)
