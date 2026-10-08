from __future__ import annotations

import argparse
import asyncio
import logging
import os
import signal
import sys
from pathlib import Path

from aiohttp import web
from musegadget import config, identity
from musegadget.executor import Account, Executor

from .broker import BridgeBroker
from .http import create_app
from .session import BridgeService

DEFAULT_TOKEN_FILE = Path("/etc/coyote-muse-bridge/token")
TOKEN_ENV = "COYOTE_MUSE_BRIDGE_TOKEN"


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(prog="coyote-muse-bridge")
    parser.add_argument(
        "--bind", default="127.0.0.1",
        help="listen address (default: loopback; use 0.0.0.0 explicitly for LAN)",
    )
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--token-file", type=Path, default=DEFAULT_TOKEN_FILE)
    parser.add_argument(
        "--run-as", default=os.environ.get("MUSEGADGET_RUN_AS") or os.environ.get("SUDO_USER"),
        help="account whose permissions Muse commands use (required when running as root)",
    )
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args(argv)


def load_bridge_token(path: Path) -> str:
    token = os.environ.get(TOKEN_ENV)
    if token is None:
        try:
            token = path.read_text(encoding="utf-8")
        except OSError as exc:
            raise ValueError("bridge bearer token is not configured") from exc
    token = token.strip()
    if not token:
        raise ValueError("bridge bearer token is empty")
    return token


async def run(args: argparse.Namespace, bridge_token: str) -> None:
    service: BridgeService | None = None
    broker = BridgeBroker(lambda: service._current if service is not None else None)
    try:
        sdk_token = config.sdk_token()
    except ValueError as exc:
        logging.getLogger(__name__).warning("running without a valid Muse SDK token: %s", exc)
        sdk_token = None
    if os.geteuid() == 0:
        if not args.run_as:
            raise ValueError("--run-as is required when running as root")
        try:
            account = Account.lookup(args.run_as)
        except KeyError as exc:
            raise ValueError(f"run-as account {args.run_as!r} does not exist") from exc
    else:
        account = Account.current()
    service = BridgeService(
        identity=identity.load_or_create(),
        executor=Executor(account),
        sdk_token=sdk_token,
        event_sink=broker.feed_event,
        subscription_error_sink=broker.session_lost,
    )

    runner = web.AppRunner(create_app(broker, bridge_token), access_log=None)
    await runner.setup()
    site = web.TCPSite(runner, args.bind, args.port)
    await site.start()
    logging.getLogger(__name__).info("listening on %s:%d", args.bind, args.port)

    loop = asyncio.get_running_loop()
    for signum in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(signum, service.stop)
    try:
        await service.run()
    finally:
        broker.session_lost()
        await runner.cleanup()


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    try:
        token = load_bridge_token(args.token_file)
    except ValueError as exc:
        print(f"coyote-muse-bridge: {exc}", file=sys.stderr)
        return 2
    try:
        asyncio.run(run(args, token))
    except ValueError as exc:
        print(f"coyote-muse-bridge: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
