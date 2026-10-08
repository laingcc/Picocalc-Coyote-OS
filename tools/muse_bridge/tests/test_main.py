from __future__ import annotations

import os

import pytest

from coyote_muse_bridge.__main__ import load_bridge_token, parse_args


def test_cli_defaults_to_loopback_and_lan_requires_explicit_bind(tmp_path, monkeypatch):
    args = parse_args([])
    assert (args.bind, args.port) == ("127.0.0.1", 8765)
    assert parse_args(["--bind", "0.0.0.0"]).bind == "0.0.0.0"

    token_file = tmp_path / "token"
    file_token = "f" * 32
    env_token = "e" * 32
    token_file.write_text(file_token + "\n")
    monkeypatch.delenv("COYOTE_MUSE_BRIDGE_TOKEN", raising=False)
    assert load_bridge_token(token_file) == file_token
    monkeypatch.setenv("COYOTE_MUSE_BRIDGE_TOKEN", env_token)
    assert load_bridge_token(token_file) == env_token


def test_empty_or_missing_bridge_token_is_rejected(tmp_path, monkeypatch):
    monkeypatch.delenv("COYOTE_MUSE_BRIDGE_TOKEN", raising=False)
    with pytest.raises(ValueError):
        load_bridge_token(tmp_path / "missing")
    empty = tmp_path / "empty"
    empty.write_text("\n")
    with pytest.raises(ValueError):
        load_bridge_token(empty)
    short = tmp_path / "short"
    short.write_text("too-short")
    with pytest.raises(ValueError):
        load_bridge_token(short)


def test_bridge_token_strength_is_measured_in_utf8_bytes(tmp_path, monkeypatch):
    token_file = tmp_path / "utf8-token"
    token_file.write_text("🧩" * 8)
    monkeypatch.delenv("COYOTE_MUSE_BRIDGE_TOKEN", raising=False)
    assert load_bridge_token(token_file) == "🧩" * 8

    token_file.write_text("🧩" * 7)
    with pytest.raises(ValueError):
        load_bridge_token(token_file)
