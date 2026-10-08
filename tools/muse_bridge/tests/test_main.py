from __future__ import annotations

import os

import pytest

from coyote_muse_bridge.__main__ import load_bridge_token, parse_args


def test_cli_defaults_to_loopback_and_lan_requires_explicit_bind(tmp_path, monkeypatch):
    args = parse_args([])
    assert (args.bind, args.port) == ("127.0.0.1", 8765)
    assert parse_args(["--bind", "0.0.0.0"]).bind == "0.0.0.0"

    token_file = tmp_path / "token"
    token_file.write_text("file-secret\n")
    monkeypatch.delenv("COYOTE_MUSE_BRIDGE_TOKEN", raising=False)
    assert load_bridge_token(token_file) == "file-secret"
    monkeypatch.setenv("COYOTE_MUSE_BRIDGE_TOKEN", "env-secret")
    assert load_bridge_token(token_file) == "env-secret"


def test_empty_or_missing_bridge_token_is_rejected(tmp_path, monkeypatch):
    monkeypatch.delenv("COYOTE_MUSE_BRIDGE_TOKEN", raising=False)
    with pytest.raises(ValueError):
        load_bridge_token(tmp_path / "missing")
    empty = tmp_path / "empty"
    empty.write_text("\n")
    with pytest.raises(ValueError):
        load_bridge_token(empty)
