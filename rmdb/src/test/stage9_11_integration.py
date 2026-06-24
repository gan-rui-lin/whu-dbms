#!/usr/bin/env python3

import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import time


PORT = 8765


def wait_for_server(proc: subprocess.Popen) -> None:
    deadline = time.time() + 8
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"server exited early with code {proc.returncode}")
        try:
            with socket.create_connection(("127.0.0.1", PORT), timeout=0.1):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start")


def start_server(binary: str, workdir: pathlib.Path, database: str) -> subprocess.Popen:
    proc = subprocess.Popen(
        [binary, database], cwd=workdir, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT
    )
    wait_for_server(proc)
    return proc


def connect() -> socket.socket:
    return socket.create_connection(("127.0.0.1", PORT), timeout=3)


def sql(sock: socket.socket, statement: str) -> str:
    sock.sendall(statement.encode() + b"\0")
    chunks = bytearray()
    while b"\0" not in chunks:
        data = sock.recv(8192)
        if not data:
            raise RuntimeError(f"connection closed while executing: {statement}")
        chunks.extend(data)
    return bytes(chunks).split(b"\0", 1)[0].decode(errors="replace")


def assert_rows(output: str, expected: list[tuple[int, int]]) -> None:
    assert f"Total record(s): {len(expected)}" in output, output
    for left, right in expected:
        assert f"| {left:16d} | {right:16d} |" in output, output


def stop_server(proc: subprocess.Popen) -> None:
    try:
        with connect() as sock:
            sock.sendall(b"safe_exit\0")
    except OSError:
        pass
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.terminate()
        proc.wait(timeout=5)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: stage9_11_integration.py <rmdb-binary>")
    binary = str(pathlib.Path(sys.argv[1]).resolve())
    workdir = pathlib.Path(tempfile.mkdtemp(prefix="rmdb-stage9-11-"))
    database = "integration_db"
    proc = None
    try:
        proc = start_server(binary, workdir, database)
        with connect() as client:
            sql(client, "create table t (id int, v int);")
            sql(client, "create index t(id);")
            sql(client, "insert into t values (1, 10);")

            sql(client, "begin;")
            sql(client, "insert into t values (2, 99);")
            sql(client, "delete from t where id = 2;")
            sql(client, "abort;")
            assert_rows(sql(client, "select * from t order by id;"), [(1, 10)])

            sql(client, "begin;")
            sql(client, "update t set v = 15 where id = 1;")
            sql(client, "commit;")
            assert_rows(sql(client, "select * from t where id = 1;"), [(1, 15)])

        reader = connect()
        writer = connect()
        try:
            sql(reader, "begin;")
            assert_rows(sql(reader, "select * from t where id = 1;"), [(1, 15)])
            sql(writer, "begin;")
            assert sql(writer, "update t set v = 20 where id = 1;") == "abort\n"
            sql(reader, "commit;")
            assert_rows(sql(writer, "select * from t where id = 1;"), [(1, 15)])
        finally:
            reader.close()
            writer.close()

        with connect() as client:
            sql(client, "begin;")
            sql(client, "insert into t values (2, 20);")
            sql(client, "commit;")
            sql(client, "begin;")
            sql(client, "update t set v = 99 where id = 1;")
            sql(client, "delete from t where id = 2;")
            client.sendall(b"crash\0")
        proc.wait(timeout=5)
        proc = None

        proc = start_server(binary, workdir, database)
        with connect() as client:
            assert_rows(sql(client, "select * from t order by id;"), [(1, 15), (2, 20)])
            assert_rows(sql(client, "select * from t where id = 1;"), [(1, 15)])
            assert_rows(sql(client, "select * from t where id = 2;"), [(2, 20)])
            sql(client, "insert into t values (3, 30);")
            assert_rows(sql(client, "select * from t order by id;"), [(1, 15), (2, 20), (3, 30)])
        stop_server(proc)
        proc = None
    finally:
        if proc is not None and proc.poll() is None:
            stop_server(proc)
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    main()
