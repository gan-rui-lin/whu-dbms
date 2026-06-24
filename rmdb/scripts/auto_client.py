#!/usr/bin/env python3

import argparse
import pathlib
import socket
import subprocess
import time


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8765


def wait_for_server(proc: subprocess.Popen, host: str, port: int) -> None:
    deadline = time.time() + 8
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"server exited early with code {proc.returncode}")
        try:
            with socket.create_connection((host, port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start")


def start_server(binary: pathlib.Path, workdir: pathlib.Path, database: str, host: str, port: int) -> subprocess.Popen:
    proc = subprocess.Popen(
        [str(binary), database],
        cwd=workdir,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.STDOUT,
    )
    wait_for_server(proc, host, port)
    return proc


def connect(host: str, port: int) -> socket.socket:
    return socket.create_connection((host, port), timeout=3)


def send_sql(sock: socket.socket, statement: str) -> str:
    statement = statement.strip()
    if not statement:
        return ""
    sock.sendall(statement.encode() + b"\0")
    chunks = bytearray()
    while b"\0" not in chunks:
        data = sock.recv(8192)
        if not data:
            raise RuntimeError(f"connection closed while executing: {statement}")
        chunks.extend(data)
    return bytes(chunks).split(b"\0", 1)[0].decode(errors="replace")


def split_sql_script(text: str) -> list[str]:
    statements: list[str] = []
    current: list[str] = []
    in_string = False
    i = 0
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if not in_string and ch == "-" and nxt == "-":
            while i < len(text) and text[i] not in "\r\n":
                i += 1
            continue
        if ch == "'":
            current.append(ch)
            if in_string and nxt == "'":
                current.append(nxt)
                i += 2
                continue
            in_string = not in_string
            i += 1
            continue
        if ch == ";" and not in_string:
            statement = "".join(current).strip()
            if statement:
                statements.append(statement)
            current = []
            i += 1
            continue
        current.append(ch)
        i += 1
    tail = "".join(current).strip()
    if tail:
        statements.append(tail)
    return statements


def load_statements(args: argparse.Namespace) -> list[str]:
    if args.sql_file is not None:
        return split_sql_script(args.sql_file.read_text())
    if args.sql:
        return args.sql
    return []


def stop_server(proc: subprocess.Popen, host: str, port: int) -> None:
    try:
        with connect(host, port) as sock:
            sock.sendall(b"safe_exit\0")
    except OSError:
        pass
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.terminate()
        proc.wait(timeout=5)


def run_batch(client: socket.socket, statements: list[str]) -> None:
    for statement in statements:
        output = send_sql(client, statement)
        if output:
            print(output, end="" if output.endswith("\n") else "\n")


def run_interactive(client: socket.socket) -> None:
    while True:
        try:
            line = input("sql> ")
        except EOFError:
            break
        statement = line.strip()
        if not statement:
            continue
        if statement.lower() in {"exit", "exit;", "bye", "bye;"}:
            break
        output = send_sql(client, statement)
        if output:
            print(output, end="" if output.endswith("\n") else "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description="Start RMDB and run SQL automatically")
    parser.add_argument(
        "--server-bin",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1] / "build" / "bin" / "rmdb",
        help="path to the rmdb server binary",
    )
    parser.add_argument("--database", required=True, help="database name passed to the server")
    parser.add_argument(
        "--workdir",
        type=pathlib.Path,
        default=pathlib.Path.cwd(),
        help="working directory used to store the database files",
    )
    parser.add_argument("--host", default=DEFAULT_HOST, help="server host, default 127.0.0.1")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="server port, default 8765")
    parser.add_argument("--sql-file", type=pathlib.Path, help="SQL file to execute")
    parser.add_argument(
        "--sql",
        action="append",
        default=[],
        help="SQL statement to execute; repeat this option for multiple statements",
    )
    args = parser.parse_args()

    server_bin = args.server_bin.resolve()
    if not server_bin.exists():
        raise SystemExit(f"server binary not found: {server_bin}")

    workdir = args.workdir.resolve()
    workdir.mkdir(parents=True, exist_ok=True)

    statements = load_statements(args)
    proc = None
    try:
        proc = start_server(server_bin, workdir, args.database, args.host, args.port)
        with connect(args.host, args.port) as client:
            if statements:
                run_batch(client, statements)
            else:
                run_interactive(client)
    finally:
        if proc is not None and proc.poll() is None:
            stop_server(proc, args.host, args.port)


if __name__ == "__main__":
    main()