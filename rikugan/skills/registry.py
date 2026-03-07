"""Skill registry: discover, query, and resolve skill invocations."""

from __future__ import annotations

import os
from typing import Dict, List, Optional, Tuple

from ..core.config import RikuganConfig
from ..core.logging import log_debug, log_info
from .loader import SkillDefinition, discover_skills


class SkillRegistry:
    """Central registry of available skills.

    Discovers skills from two locations:
    1. Built-in skills shipped with Rikugan (rikugan/skills/builtins/)
    2. User skills (``~/.idapro/rikugan/skills/`` via RikuganConfig.skills_dir)

    User skills with the same slug override built-in ones.
    """

    def __init__(self, skills_dir: str = ""):
        if not skills_dir:
            skills_dir = RikuganConfig().skills_dir
        self._skills_dir = skills_dir
        self._skills: Dict[str, SkillDefinition] = {}

    def discover(self) -> int:
        """Scan built-in and user skills directories. Returns total count."""
        self._skills.clear()

        # 1. Built-in skills (ship with plugin)
        builtins_dir = os.path.join(os.path.dirname(__file__), "builtins")
        builtin_skills = discover_skills(builtins_dir)
        for skill in builtin_skills:
            self._skills[skill.slug] = skill
        log_info(f"Discovered {len(builtin_skills)} built-in skills")

        # 2. User skills (override built-ins with same slug)
        user_skills = discover_skills(self._skills_dir)
        overrides = 0
        for skill in user_skills:
            if skill.slug in self._skills:
                overrides += 1
                log_debug(f"User skill /{skill.slug} overrides built-in")
            self._skills[skill.slug] = skill
        if user_skills:
            log_info(f"Discovered {len(user_skills)} user skills ({overrides} overrides)")

        log_info(f"Total skills available: {len(self._skills)}")
        return len(self._skills)

    def get(self, slug: str) -> Optional[SkillDefinition]:
        return self._skills.get(slug)

    def list_skills(self) -> List[SkillDefinition]:
        return list(self._skills.values())

    def list_slugs(self) -> List[str]:
        return list(self._skills.keys())

    def get_summary_for_prompt(self) -> Optional[str]:
        """Format a summary for inclusion in the system prompt."""
        if not self._skills:
            return None
        lines = ["Available skills (user invokes with /slug, or you can call activate_skill):"]
        for slug, skill in sorted(self._skills.items()):
            desc = skill.description or "(no description)"
            lines.append(f"  - /{slug}: {desc}")
        return "\n".join(lines)

    def match_triggers(self, user_text: str) -> Optional[SkillDefinition]:
        """Match user text against skill trigger patterns.

        Returns the best-matching skill, or None if no triggers match.
        Skills with more matching triggers are preferred.
        """
        text_lower = user_text.lower()
        best_skill: Optional[SkillDefinition] = None
        best_count = 0

        for skill in self._skills.values():
            if not skill.triggers:
                continue
            hits = sum(1 for t in skill.triggers if t in text_lower)
            if hits > best_count:
                best_count = hits
                best_skill = skill

        if best_skill:
            log_debug(f"Trigger match: /{best_skill.slug} ({best_count} hits)")
        return best_skill

    def resolve_skill_invocation(self, user_text: str) -> Tuple[Optional[SkillDefinition], str]:
        """Check if user_text starts with /slug. Returns (skill, remaining) or (None, user_text)."""
        text = user_text.strip()
        if not text.startswith("/"):
            return (None, user_text)

        # Extract the slug: everything from / to the first whitespace
        parts = text[1:].split(None, 1)
        if not parts:
            return (None, user_text)

        slug = parts[0]
        remaining = parts[1] if len(parts) > 1 else ""

        skill = self._skills.get(slug)
        if skill is None:
            log_debug(f"No skill found for slug: {slug}")
            return (None, user_text)

        log_debug(f"Resolved skill invocation: /{slug}")
        return (skill, remaining)
