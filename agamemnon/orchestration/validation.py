"""Input validation utilities for safe URL path construction."""

from __future__ import annotations

_MAX_ID_LENGTH = 256


def validate_id(value: str, field_name: str) -> str:
    """Validate an ID string for safe use in URL path segments.

    Rejects empty strings, strings containing path separators (``/``, ``\\``,
    ``..``), whitespace, control characters, and strings longer than 256
    characters.

    Args:
        value: The ID string to validate.
        field_name: Human-readable name of the field (used in error messages).

    Returns:
        The original ``value`` unchanged if it passes all checks.

    Raises:
        ValueError: If ``value`` fails any validation check.
    """
    if not value or not value.strip():
        raise ValueError(f"{field_name} must not be empty or whitespace-only")

    if len(value) > _MAX_ID_LENGTH:
        raise ValueError(f"{field_name} exceeds maximum length of {_MAX_ID_LENGTH} characters")

    for ch in value:
        if ch.isspace() or ord(ch) < 32:
            raise ValueError(f"{field_name} contains invalid character: {ch!r}")

    if "/" in value or "\\" in value:
        raise ValueError(f"{field_name} contains path separator characters")

    if ".." in value:
        raise ValueError(f"{field_name} contains path traversal sequence '..'")

    return value
