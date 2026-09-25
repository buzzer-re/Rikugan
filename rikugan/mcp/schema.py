"""Make an MCP server's JSON Schema safe to hand to a model provider.

A server describes its tools in full JSON Schema, including the parts that
only make sense inside its own document: ``$ref`` pointers into a ``$defs``
section, and keywords no provider evaluates. Relaying those verbatim sends a
schema that cannot be resolved — a ``$ref`` whose target was left behind is
not merely ignored, it makes the whole tool declaration invalid, and one bad
tool fails the entire request rather than just itself.

So references are resolved against the document they came from and inlined,
anything still dangling is dropped, and the result is restricted to keywords
that describe a value rather than a document.
"""

from __future__ import annotations

from typing import Any

# Keywords that describe the shape of a value. Everything else — $id, $schema,
# $comment, definitions plumbing, unevaluated*, conditional applicators — says
# something about the document, not about what the model should produce.
_VALUE_KEYWORDS = frozenset(
    {
        "type",
        "description",
        "title",
        "default",
        "enum",
        "const",
        "format",
        "properties",
        "required",
        "items",
        "prefixItems",
        "additionalProperties",
        "anyOf",
        "oneOf",
        "allOf",
        "minimum",
        "maximum",
        "exclusiveMinimum",
        "exclusiveMaximum",
        "minLength",
        "maxLength",
        "minItems",
        "maxItems",
        "uniqueItems",
        "pattern",
        "nullable",
    }
)

_SUBSCHEMA_KEYWORDS = ("properties", "items", "prefixItems", "additionalProperties")
_COMBINATOR_KEYWORDS = ("anyOf", "oneOf", "allOf")

# A self-referential type (a tree node, say) would otherwise inline forever.
_MAX_DEPTH = 6


def collect_defs(schema: dict[str, Any]) -> dict[str, Any]:
    """Return the definition sections a ``$ref`` in this document can point at."""
    defs: dict[str, Any] = {}
    for key in ("$defs", "definitions"):
        section = schema.get(key)
        if isinstance(section, dict):
            defs.update(section)
    return defs


def _resolve(ref: str, defs: dict[str, Any]) -> dict[str, Any] | None:
    """Look up a local ``#/$defs/Name`` pointer. Anything else is not ours to fetch."""
    if not ref.startswith("#/"):
        return None  # a remote or absolute reference; we are not fetching it
    name = ref.rsplit("/", 1)[-1]
    target = defs.get(name)
    return target if isinstance(target, dict) else None


def sanitize_schema(
    schema: Any,
    defs: dict[str, Any] | None = None,
    depth: int = 0,
) -> dict[str, Any]:
    """Return *schema* with references inlined and document keywords removed.

    Returns a permissive empty schema rather than raising when a schema cannot
    be represented: an unconstrained parameter still lets the tool be called,
    where an invalid one takes the whole request down with it.
    """
    if not isinstance(schema, dict) or depth > _MAX_DEPTH:
        return {}

    defs = defs if defs is not None else collect_defs(schema)
    if "$ref" in schema:
        target = _resolve(str(schema["$ref"]), defs)
        if target is None:
            return {}
        # The pointer may carry siblings (a description of its own); the
        # target supplies the shape, the siblings refine it.
        merged = dict(target)
        merged.update({k: v for k, v in schema.items() if k != "$ref"})
        return sanitize_schema(merged, defs, depth + 1)

    out: dict[str, Any] = {}
    for key, value in schema.items():
        if key not in _VALUE_KEYWORDS:
            continue
        if key == "properties" and isinstance(value, dict):
            out[key] = {name: sanitize_schema(sub, defs, depth + 1) for name, sub in value.items()}
        elif key in _COMBINATOR_KEYWORDS and isinstance(value, list):
            branches = [sanitize_schema(sub, defs, depth + 1) for sub in value]
            branches = [b for b in branches if b]
            if branches:
                out[key] = branches
        elif key in _SUBSCHEMA_KEYWORDS and isinstance(value, dict):
            out[key] = sanitize_schema(value, defs, depth + 1)
        elif key == "prefixItems" and isinstance(value, list):
            out[key] = [sanitize_schema(sub, defs, depth + 1) for sub in value]
        else:
            out[key] = value

    # An array with no item shape left is still an array; saying so beats
    # leaving a key that points nowhere.
    if out.get("type") == "array" and not out.get("items"):
        out.pop("items", None)
    return out
