"""Local SQLite history. Operations are serialized by Dashboard's lock."""
import csv
import json
import sqlite3
from contextlib import closing
from pathlib import Path

from protocol import COMMAND_SECONDS, TRAYS

SCHEMA = """
PRAGMA journal_mode = WAL;
CREATE TABLE IF NOT EXISTS samples (
    id INTEGER PRIMARY KEY, at REAL NOT NULL, boot_id TEXT NOT NULL, seq INTEGER NOT NULL,
    gap INTEGER NOT NULL, dS INTEGER, dM INTEGER, dL INTEGER, payload TEXT NOT NULL,
    UNIQUE (boot_id, seq)
);
CREATE INDEX IF NOT EXISTS samples_at ON samples(at);
CREATE TABLE IF NOT EXISTS events (
    at REAL NOT NULL, boot_id TEXT NOT NULL, event_id TEXT NOT NULL, payload TEXT NOT NULL,
    PRIMARY KEY (boot_id, event_id)
);
CREATE INDEX IF NOT EXISTS events_at ON events(at);
CREATE TABLE IF NOT EXISTS commands (
    request_id TEXT PRIMARY KEY, at REAL NOT NULL, link_id TEXT NOT NULL,
    state TEXT NOT NULL, reason TEXT NOT NULL, payload TEXT NOT NULL,
    ack_at REAL, ack_mode TEXT
);
CREATE INDEX IF NOT EXISTS commands_at ON commands(at);
CREATE TABLE IF NOT EXISTS runs (
    id INTEGER PRIMARY KEY, name TEXT NOT NULL, started_at REAL NOT NULL, ended_at REAL
);
CREATE UNIQUE INDEX IF NOT EXISTS one_open_run ON runs((1)) WHERE ended_at IS NULL;
CREATE TABLE IF NOT EXISTS console_logs (
    id INTEGER PRIMARY KEY, at REAL NOT NULL, message TEXT NOT NULL
);
"""


class Store:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.path = path
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.executescript(SCHEMA)
        with self.db:
            self.db.execute("UPDATE commands SET state='unknown', reason='BACKEND_RESTART' WHERE state='pending'")

    def latest(self):
        row = self.db.execute("SELECT at, payload FROM samples ORDER BY id DESC LIMIT 1").fetchone()
        return (row["at"], json.loads(row["payload"])) if row else (None, None)

    def record_log(self, at, message):
        with self.db:
            self.db.execute("INSERT INTO console_logs(at,message) VALUES(?,?)", (at, message))

    def recent_logs(self, limit):
        rows = self.db.execute("SELECT id,at,message FROM console_logs ORDER BY id DESC LIMIT ?", (limit,))
        return [dict(row) for row in reversed(rows.fetchall())]

    def record_sample(self, at, sample, previous, gap):
        current_counts = sample.trays.model_dump()
        prior_counts = previous.trays.model_dump() if previous and previous.boot_id == sample.boot_id else {}
        deltas = []
        for tray in TRAYS:
            current = current_counts[tray]
            prior = prior_counts.get(tray, 0)
            deltas.append(current - prior if previous and current is not None and prior is not None
                          and current >= prior else None)
        with self.db:
            result = self.db.execute(
                "INSERT OR IGNORE INTO samples(at, boot_id, seq, gap, dS, dM, dL, payload) VALUES(?,?,?,?,?,?,?,?)",
                (at, sample.boot_id, sample.seq, gap, *deltas, sample.model_dump_json()),
            )
        return result.rowcount == 1

    def record_event(self, at, event):
        with self.db:
            self.db.execute("INSERT OR IGNORE INTO events VALUES(?,?,?,?)",
                            (at, event.boot_id, event.event_id, event.model_dump_json()))

    def record_command(self, at, command):
        with self.db:
            self.db.execute(
                "INSERT INTO commands(request_id,at,link_id,state,reason,payload) VALUES(?,?,?,'pending','WAITING_ACK',?)",
                (command.request_id, at, command.link_id, command.model_dump_json()),
            )

    def uncertain(self, request_id, reason):
        with self.db:
            self.db.execute("UPDATE commands SET state='unknown', reason=? WHERE request_id=? AND state='pending'",
                            (reason, request_id))

    def record_ack(self, at, ack):
        with self.db:
            self.db.execute("""UPDATE commands SET state=?, reason=?, ack_at=?, ack_mode=?
                WHERE request_id=? AND link_id=? AND state IN ('pending','unknown')""",
                (ack.result, ack.reason, at, ack.mode, ack.request_id, ack.link_id))

    def expire_commands(self, at):
        with self.db:
            self.db.execute("UPDATE commands SET state='unknown', reason='ACK_TIMEOUT' WHERE state='pending' AND at<=?",
                            (at - COMMAND_SECONDS,))

    def in_flight(self, at, action):
        # Allow a pause to preempt other work. Unknown commands may still be in transit.
        return self.db.execute("""SELECT 1 FROM commands WHERE state IN ('pending','unknown')
            AND at>? AND (? != 'PAUSE_SORTING' OR json_extract(payload,'$.action')='PAUSE_SORTING') LIMIT 1""",
            (at - COMMAND_SECONDS, action)).fetchone() is not None

    @staticmethod
    def command_dict(row):
        request = json.loads(row["payload"])
        return {key: row[key] for key in ("request_id", "at", "state", "reason", "ack_at", "ack_mode")} | {
            "action": request["action"], "station": request["station"],
        }

    def command(self, request_id):
        row = self.db.execute("SELECT * FROM commands WHERE request_id=?", (request_id,)).fetchone()
        return self.command_dict(row) if row else None

    def runs(self):
        return [dict(row) for row in self.db.execute("SELECT * FROM runs ORDER BY id DESC")]

    def run(self, run_id):
        row = self.db.execute("SELECT * FROM runs WHERE id=?", (run_id,)).fetchone()
        return dict(row) if row else None

    def active_run(self):
        row = self.db.execute("SELECT * FROM runs WHERE ended_at IS NULL").fetchone()
        return dict(row) if row else None

    def start_run(self, name, at):
        with self.db:
            cursor = self.db.execute("INSERT INTO runs(name, started_at) VALUES(?,?)", (name, at))
        return self.run(cursor.lastrowid)

    def end_run(self, run_id, at):
        with self.db:
            cursor = self.db.execute("UPDATE runs SET ended_at=? WHERE id=? AND ended_at IS NULL", (at, run_id))
        return self.run(run_id) if cursor.rowcount else None

    def run_totals(self, run, now, current):
        if not run:
            return {tray: None for tray in TRAYS}
        row = self.db.execute("""SELECT SUM(dS) S, SUM(dM) M, SUM(dL) L FROM samples
            WHERE at>? AND at<=?""", (run["started_at"], run["ended_at"] or now)).fetchone()
        return {tray: (row[tray] or 0) if current[tray] is not None else None for tray in TRAYS}

    def recent_rate(self, latest_at, latest):
        before = self.db.execute("""SELECT id, at FROM samples WHERE boot_id=? AND at<=?
            ORDER BY at DESC LIMIT 1""", (latest.boot_id, latest_at - 10)).fetchone()
        if not before or before["at"] < latest_at - 13:
            return None
        rows = list(self.db.execute("SELECT at,gap,dS,dM,dL FROM samples WHERE id>? ORDER BY id", (before["id"],)))
        if any(row["gap"] for row in rows) or not rows:
            return None
        usable = [tray for tray in TRAYS if getattr(latest.trays, tray) is not None]
        if not usable or any(row["d" + tray] is None for row in rows for tray in usable):
            return None
        count = sum(row["d" + tray] for row in rows for tray in usable)
        return round(count / (latest_at - before["at"]), 2)

    def history(self, start, end, limit):
        rows = list(self.db.execute("""SELECT at,boot_id,gap,dS,dM,dL,payload FROM samples
            WHERE at>? AND at<=? ORDER BY id DESC LIMIT ?""", (start, end, limit)))
        points = [
            {"at": row["at"], "boot_id": row["boot_id"], "gap": bool(row["gap"]),
             "counts": {tray: row["d" + tray] for tray in TRAYS},
             "totals": json.loads(row["payload"])["trays"]}
            for row in reversed(rows)
        ]
        events = [{"at": row["at"], **json.loads(row["payload"])} for row in self.db.execute(
            "SELECT at,payload FROM events WHERE at>? AND at<=? ORDER BY at DESC LIMIT ?", (start, end, limit)
        )]
        commands = [self.command_dict(row) for row in self.db.execute(
            "SELECT * FROM commands WHERE at>? AND at<=? ORDER BY at DESC LIMIT ?", (start, end, limit)
        )]
        return {"points": points, "events": events, "commands": commands}

    def export(self, start, end):
        # The streaming reader has its own snapshot and cannot block the live connection.
        with closing(sqlite3.connect(self.path)) as reader:
            rows = reader.execute("""
                SELECT 'sample' AS type, at, boot_id AS key, gap, dS, dM, dL, payload, NULL, NULL, NULL, NULL
                    FROM samples WHERE at>? AND at<=?
                UNION ALL
                SELECT 'event', at, event_id, NULL, NULL, NULL, NULL, payload, NULL, NULL, NULL, NULL
                    FROM events WHERE at>? AND at<=?
                UNION ALL
                SELECT 'command', at, request_id, NULL, NULL, NULL, NULL, payload, state, reason, ack_at, ack_mode
                    FROM commands WHERE at>? AND at<=?
                ORDER BY at
            """, (start, end, start, end, start, end))
            writer = csv.writer(CsvLine())
            yield "\ufeff" + writer.writerow(("type", "received_at_unix", "id", "gap", "delta_S", "delta_M",
                                               "delta_L", "payload_json", "state", "reason", "ack_at_unix", "ack_mode"))
            for row in rows:
                values = list(row)
                if values[9] and values[9].lstrip().startswith(("=", "+", "-", "@")):
                    values[9] = "'" + values[9]
                yield writer.writerow(values)


class CsvLine:
    def write(self, value):
        return value

