"""Unit tests for validate_id() in agamemnon.orchestration.validation."""

from __future__ import annotations

import pytest
from agamemnon.orchestration.validation import validate_id


class TestValidateIdAccepts:
    """validate_id() should return the value unchanged for valid inputs."""

    @pytest.mark.parametrize("value", [
        "550e8400-e29b-41d4-a716-446655440000",  # standard UUID
        "team-1",
        "abc123",
        "T",
        "team.alpha",
        "a" * 256,  # exactly at max length
    ])
    def test_valid_ids_pass(self, value: str) -> None:
        assert validate_id(value, "team_id") == value

    def test_returns_original_string(self) -> None:
        val = "my-team-42"
        assert validate_id(val, "team_id") is val


class TestValidateIdRejects:
    """validate_id() should raise ValueError for invalid inputs."""

    def test_empty_string(self) -> None:
        with pytest.raises(ValueError, match="empty"):
            validate_id("", "team_id")

    def test_whitespace_only(self) -> None:
        with pytest.raises(ValueError, match="whitespace"):
            validate_id("   ", "task_id")

    def test_contains_forward_slash(self) -> None:
        with pytest.raises(ValueError, match="path separator"):
            validate_id("team/evil", "team_id")

    def test_contains_backslash(self) -> None:
        with pytest.raises(ValueError, match="path separator"):
            validate_id("team\\evil", "team_id")

    def test_path_traversal_with_slash(self) -> None:
        with pytest.raises(ValueError):
            validate_id("../admin", "team_id")

    def test_path_traversal_dotdot_no_slash(self) -> None:
        with pytest.raises(ValueError, match="path traversal"):
            validate_id("..admin", "team_id")

    def test_path_traversal_embedded(self) -> None:
        with pytest.raises(ValueError, match="path traversal"):
            validate_id("team..id", "team_id")

    def test_contains_newline(self) -> None:
        with pytest.raises(ValueError, match="invalid character"):
            validate_id("team\nid", "team_id")

    def test_contains_tab(self) -> None:
        with pytest.raises(ValueError, match="invalid character"):
            validate_id("team\tid", "team_id")

    def test_contains_null_byte(self) -> None:
        with pytest.raises(ValueError, match="invalid character"):
            validate_id("team\x00id", "task_id")

    def test_contains_control_character(self) -> None:
        with pytest.raises(ValueError, match="invalid character"):
            validate_id("team\x1fid", "task_id")

    def test_exceeds_max_length(self) -> None:
        with pytest.raises(ValueError, match="exceeds maximum length"):
            validate_id("a" * 257, "team_id")

    def test_error_message_includes_field_name(self) -> None:
        with pytest.raises(ValueError, match="my_field"):
            validate_id("", "my_field")


class TestValidateIdFieldName:
    """field_name is included in all error messages."""

    @pytest.mark.parametrize("bad_value,field_name", [
        ("", "team_id"),
        ("../x", "task_id"),
        ("x/y", "some_field"),
        ("a" * 300, "record_id"),
    ])
    def test_field_name_in_error(self, bad_value: str, field_name: str) -> None:
        with pytest.raises(ValueError, match=field_name):
            validate_id(bad_value, field_name)
