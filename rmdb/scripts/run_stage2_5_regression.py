#!/usr/bin/env python3
import argparse
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path


HOST = "127.0.0.1"
PORT = 8765


def is_port_open() -> bool:
    try:
        with socket.create_connection((HOST, PORT), timeout=0.5):
            return True
    except OSError:
        return False


def request_safe_exit() -> None:
    try:
        with socket.create_connection((HOST, PORT), timeout=1.0) as conn:
            conn.sendall(b"safe_exit\0")
            try:
                conn.recv(4096)
            except OSError:
                pass
    except OSError:
        return


def wait_for_server(timeout_sec: float = 10.0, server: subprocess.Popen | None = None) -> None:
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        if server is not None and server.poll() is not None:
            output = ""
            if server.stdout is not None:
                output = server.stdout.read()
            raise RuntimeError(f"RMDB server exited early with code {server.returncode}\n{output}")
        try:
            with socket.create_connection((HOST, PORT), timeout=0.5):
                return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("RMDB server did not become ready in time")


def send_sql(conn: socket.socket, sql: str) -> str:
    conn.sendall(sql.encode("utf-8") + b"\0")
    return conn.recv(65536).decode("utf-8", "ignore").replace("\0", "")


def assert_contains(text: str, expected: str, context: str) -> None:
    if expected not in text:
        raise AssertionError(f"{context} missing expected text: {expected!r}\nActual:\n{text}")


def run_regression(build_dir: Path, db_name: str) -> Path:
    if is_port_open():
        request_safe_exit()
        deadline = time.time() + 5.0
        while is_port_open() and time.time() < deadline:
            time.sleep(0.1)
        if is_port_open():
            raise RuntimeError("port 8765 is still occupied before regression start")

    db_dir = build_dir / db_name
    if db_dir.exists():
        shutil.rmtree(db_dir)

    server = subprocess.Popen(
        [str(build_dir / "bin" / "rmdb"), db_name],
        cwd=build_dir,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    try:
        wait_for_server(server=server)
        with socket.create_connection((HOST, PORT), timeout=5) as conn:
            # Stage 2
            for sql in [
                "create table grade (name char(4),id int,score float);",
                "insert into grade values ('Data', 1, 90.5);",
                "insert into grade values ('Data', 2, 95.0);",
                "insert into grade values ('Calc', 2, 92.0);",
                "insert into grade values ('Calc', 1, 88.5);",
                "create table t ( id int , t_name char(3));",
                "create table d (d_name char(5),id int);",
                "insert into t values (1,'aaa');",
                "insert into t values (2,'baa');",
                "insert into t values (3,'bba');",
                "insert into d values ('12345',1);",
                "insert into d values ('23456',2);",
            ]:
                send_sql(conn, sql)

            resp = send_sql(conn, "select score,name,id from grade where score > 90;")
            assert_contains(resp, "90.500000", "stage2 float comparison")
            assert_contains(resp, "95.000000", "stage2 float comparison")
            assert_contains(resp, "92.000000", "stage2 float comparison")

            resp = send_sql(conn, "select name from grade where id = 2 and score > 90;")
            assert_contains(resp, "Data", "stage2 mixed predicate")
            assert_contains(resp, "Calc", "stage2 mixed predicate")

            send_sql(conn, "update grade set score = 99.0 where name = 'Calc';")
            send_sql(conn, "delete from grade where score > 98;")
            resp = send_sql(conn, "select * from grade;")
            if "99.000000" in resp:
                raise AssertionError("stage2 delete regression failed: deleted rows still present")
            assert_contains(resp, "90.500000", "stage2 delete regression")

            resp = send_sql(conn, "select t.id,t_name,d_name from t,d where t.id = d.id;")
            assert_contains(resp, "12345", "stage2 join")
            assert_contains(resp, "23456", "stage2 join")

            # Stage 3
            for sql in [
                "create table t_big(bid bigint,sid int);",
                "insert into t_big values(372036854775807,233421);",
                "insert into t_big values(-922337203685477580,124332);",
            ]:
                send_sql(conn, sql)
            resp = send_sql(conn, "insert into t_big values(9223372036854775809,12345);")
            assert_contains(resp, "out of range", "stage3 overflow")
            resp = send_sql(conn, "select * from t_big;")
            assert_contains(resp, "372036854775807", "stage3 bigint select")
            assert_contains(resp, "124332", "stage3 bigint select")

            # Stage 4
            for sql in [
                "create table t_dt(id int , time datetime);",
                "insert into t_dt values(1, '2023-05-18 09:12:19');",
                "insert into t_dt values(2, '2023-05-30 12:34:32');",
                "delete from t_dt where time = '2023-05-30 12:34:32';",
                "update t_dt set id = 2023 where time = '2023-05-18 09:12:19';",
                "create table t_bad(time datetime, temperature float);",
                "insert into t_bad values('1999-07-07 12:30:00' , 36.0);",
            ]:
                send_sql(conn, sql)

            resp = send_sql(conn, "select * from t_dt;")
            assert_contains(resp, "2023", "stage4 datetime update")
            assert_contains(resp, "2023-05-18", "stage4 datetime update")

            for bad_sql in [
                "insert into t_bad values('1999-13-07 12:30:00' , 36.0);",
                "insert into t_bad values('1999-1-07 12:30:00' , 36.0);",
                "insert into t_bad values('1999-00-07 12:30:00' , 36.0);",
                "insert into t_bad values('1999-07-00 12:30:00' , 36.0);",
                "insert into t_bad values('0001-07-10 12:30:00' , 36.0);",
                "insert into t_bad values('1999-02-30 12:30:00' , 36.0);",
                "insert into t_bad values('1999-02-28 12:30:61' , 36.0);",
            ]:
                resp = send_sql(conn, bad_sql)
                assert_contains(resp, "Invalid datetime", "stage4 invalid datetime")
            resp = send_sql(conn, "select * from t_bad;")
            assert_contains(resp, "1999-07-07", "stage4 valid datetime remains")

            # Stage 5
            for sql in [
                "create table warehouse (w_id int, name char(8));",
                "insert into warehouse values (10 , 'qweruiop');",
                "insert into warehouse values (534, 'asdfhjkl');",
                "insert into warehouse values (100,'qwerghjk');",
                "insert into warehouse values (500,'bgtyhnmj');",
                "create index warehouse(w_id);",
            ]:
                send_sql(conn, sql)

            resp = send_sql(conn, "show index from warehouse;")
            assert_contains(resp, "warehouse", "stage5 show index")
            assert_contains(resp, "(w_id)", "stage5 show index")

            resp = send_sql(conn, "select * from warehouse where w_id = 10;")
            assert_contains(resp, "qweruiop", "stage5 point lookup")
            resp = send_sql(conn, "select * from warehouse where w_id < 534 and w_id > 100;")
            assert_contains(resp, "500", "stage5 range scan")

            for sql in [
                "drop index warehouse(w_id);",
                "create index warehouse(name);",
            ]:
                send_sql(conn, sql)

            resp = send_sql(conn, "select * from warehouse where name > 'aszdefgh' and name < 'qweraaaa';")
            assert_contains(resp, "bgtyhnmj", "stage5 string range scan")

            for sql in [
                "drop index warehouse(name);",
                "create index warehouse(w_id,name);",
            ]:
                send_sql(conn, sql)

            resp = send_sql(conn, "show index from warehouse;")
            assert_contains(resp, "(w_id,name)", "stage5 composite index")

            resp = send_sql(conn, "insert into warehouse values (10 , 'qweruiop');")
            assert_contains(resp, "Index already exists", "stage5 unique constraint")

            resp = send_sql(conn, "select from syntax_error;")
            assert_contains(resp, "parse error", "parser failure output")

            send_sql(conn, "safe_exit")
    finally:
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait(timeout=5)

    output_path = db_dir / "output.txt"
    output = output_path.read_text(encoding="utf-8")
    for expected in [
        "| score | name | id |",
        "| 372036854775807 | 233421 |",
        "| -922337203685477580 | 124332 |",
        "| 2023 | 2023-05-18 09:12:19 |",
        "| 1999-07-07 12:30:00 | 36.000000 |",
        "| warehouse | unique | (w_id) |",
        "| warehouse | unique | (w_id,name) |",
    ]:
        assert_contains(output, expected, "output.txt")

    failure_count = output.count("failure\n")
    if failure_count < 9:
        raise AssertionError(f"expected at least 9 failure lines in output.txt, got {failure_count}")

    return output_path


def main() -> int:
    parser = argparse.ArgumentParser(description="Run stage2-5 RMDB regression cases")
    parser.add_argument("--build-dir", default="build", help="RMDB build directory")
    parser.add_argument("--db-name", default="stage2_5_regression_db", help="Database directory name")
    args = parser.parse_args()

    build_dir = Path(args.build_dir).resolve()
    output_path = run_regression(build_dir, args.db_name)
    print(f"Regression passed. output.txt: {output_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
