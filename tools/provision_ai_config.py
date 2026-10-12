#!/usr/bin/env python3
"""Safely provision a PicoCalc cloud-provider key onto its SD card.

The API key is prompted through the terminal with echo disabled. It is never
accepted as a command-line argument, printed, or logged.
"""
from __future__ import annotations

import argparse
import getpass
import os
import shutil
import sys
import tempfile
from pathlib import Path

CLOUDS = {
    "deepseek": ("api.deepseek.com", "deepseek-chat", ("deepseek-chat", "deepseek-reasoner")),
    "openai": ("api.openai.com", "gpt-6-astra", ("gpt-6-astra", "gpt-6.1-sol", "gpt-6-luna")),
    "mistral": ("api.mistral.ai", "mistral-small-latest", ("mistral-small-latest", "mistral-medium-latest", "mistral-large-latest")),
    "gemini": ("generativelanguage.googleapis.com", "gemini-3.8-flash", ("gemini-3.8-flash", "gemini-3.6-flash", "gemini-3.5-flash-lite")),
    "claude": ("api.anthropic.com", "claude-opus-5-5", ("claude-opus-5-5", "claude-sonnet-5-5", "claude-haiku-5-5")),
}
DEFAULTS = {
    "version": "1", "ssid": "", "password": "", "provider": "ollama", "host": "",
    "port": "11434", "model": "", "bearer_token": "", "connect_timeout_ms": "15000",
    "request_timeout_ms": "120000", "idle_timeout_ms": "15000", "max_predict": "384",
    "system_prompt": "You are an AI on a Picocalc handheld with a tiny screen and keyboard. Be concise; avoid markdown and long code.",
    "temperature": "80", "utc_offset_minutes": "0",
}


def key_is_safe(value: str) -> bool:
    return bool(value) and len(value) < 257 and all("!" <= char <= "~" for char in value)


def load_lines(path: Path, create: bool) -> list[str]:
    if path.exists():
        return path.read_text(encoding="utf-8").splitlines()
    if not create:
        raise ValueError(f"{path} does not exist; boot/configure the PicoCalc once, or pass --create")
    return [f"{key}={value}" for key, value in DEFAULTS.items()]


def replace_values(lines: list[str], values: dict[str, str]) -> list[str]:
    seen: set[str] = set()
    output: list[str] = []
    for line in lines:
        key, sep, _ = line.partition("=")
        if sep and key in values:
            if key in seen:
                raise ValueError(f"refusing ambiguous duplicate key: {key}")
            seen.add(key)
            output.append(f"{key}={values[key]}")
        else:
            output.append(line)
    for key, value in values.items():
        if key not in seen:
            output.append(f"{key}={value}")
    return output


def atomic_write(path: Path, lines: list[str]) -> None:
    fd, temporary = tempfile.mkstemp(prefix=".ai.ini.", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
            handle.write("\n".join(lines) + "\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(temporary, 0o600)
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mount", type=Path, required=True, help="mounted PicoCalc SD-card root")
    parser.add_argument("--provider", choices=sorted(CLOUDS), required=True)
    parser.add_argument("--model", help="allowed model for that provider (defaults to its first allowed model)")
    parser.add_argument("--create", action="store_true", help="create a default ai.ini only if it is absent")
    args = parser.parse_args()

    host, default_model, allowed_models = CLOUDS[args.provider]
    model = args.model or default_model
    if model not in allowed_models:
        parser.error(f"{model!r} is not an allowed {args.provider} model: {', '.join(allowed_models)}")
    config_path = args.mount / "coyote" / "ai.ini"
    if not config_path.parent.is_dir():
        parser.error(f"not a mounted PicoCalc card: {config_path.parent} is missing")
    key = getpass.getpass(f"Paste {args.provider} API key (hidden): ")
    if not key_is_safe(key):
        print("Refused: key must be 1–256 printable non-space ASCII characters.", file=sys.stderr)
        return 2
    try:
        lines = load_lines(config_path, args.create)
        if config_path.exists():
            shutil.copy2(config_path, config_path.with_suffix(".ini.bak"))
        values = {"provider": args.provider, "host": host, "port": "443", "model": model, "bearer_token": key}
        atomic_write(config_path, replace_values(lines, values))
    finally:
        key = "\0" * len(key)
    print(f"Updated {config_path}; previous file saved as {config_path.with_suffix('.ini.bak')}.")
    print("Eject the card cleanly before putting it back in PicoCalc. The key remains plaintext on the card.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
