#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
"""Writes the libwebsockets 3.0 probe passport (WebSocket spec 16.4, milestone W0) from the FACT lines of probe runs.

    python3 -I Tools/lws_probe_passport.py --log <log> [<log> ...] --out docs/passport/<date>-lws30-probe.md
    python3 -I Tools/lws_probe_passport.py --selftest

The probe tests (Source/VaRestTests/Private/LwsProbe/) log one line per fact,
    LogVaRestLwsProbe: Display: FACT {"id", "verdict", "observed", "spec", "evidence", "test"}
and "PENDING {"id"}" right before a call that may end the process: a PENDING id with no FACT becomes "crashed" (the
Probe.VaRest.Risky.* cases run one per process, so each has its own log). Every
id in EXPECTED must be present, or the exit status is 1 and the missing ones are listed, so a probe that stopped
recording cannot pass unnoticed. Ids starting with "rig." are the harness's own checks and are left out.
"""

import argparse
import datetime
import json
import os
import platform
import re
import subprocess
import sys
import tempfile
import unittest

EXPECTED = (
    "env.lws_version", "env.openssl_version", "env.build", "env.websockets_loaded_at_start",
    "release.win64", "release.mac", "release.android_ios",
    "svc.commands_inside_wait_cancelled", "svc.commands_between_service_calls", "svc.cancel_wake_ms",
    "svc.timer_latency_ms", "svc.kill_async_ms", "svc.kill_async_ms_short_wait", "svc.kill_async_sequence",
    "svc.kill_async_close_frame",
    "timeout.default_secs", "timeout.configured_secs", "timeout.kill_pending_ms", "timeout.kill_pending_ms_short_wait",
    "timeout.kill_pending_sequence", "timeout.tcp_connect_blackhole", "timeout.default_secs_blackhole",
    "hs.unregistered_selected_bound", "hs.selected_protocol_in_filter", "hs.unregistered_selected_unbound",
    "hs.unoffered_selected", "hs.none_selected", "hs.offered_header",
    "hs.raw_origin", "hs.default_origin", "hs.origin_omitted", "hs.append_header_space", "hs.append_header_user",
    "hs.append_header_fill", "hs.host_header",
    "hs.refusal_401", "hs.refusal_403", "hs.refusal_404", "hs.refusal_429", "hs.refusal_500", "hs.refusal_503",
    "hs.refusal_sequence", "hs.callback_user_by_reason", "hs.no_upgrade_response", "hs.bad_accept",
    "hs.missing_upgrade", "hs.missing_connection", "hs.connect_refused", "hs.dns_failure",
    "hs.redirect_relative", "hs.redirect_absolute", "hs.redirect_reappends_headers", "hs.redirect_to_tls",
    "hs.redirect_downgrade", "hs.redirect_target_visible", "hs.redirect_veto", "hs.redirect_host_header",
    "hs.status_in_connection_error.refused", "hs.status_in_connection_error.connect_refused",
    "hs.status_in_connection_error.dns_failure", "hs.status_in_connection_error.no_upgrade",
    "hs.status_in_connection_error.tls_verify_failure", "hs.status_in_connection_error.killed_pending",
    "close.rewritten_codes", "close.echo_on_return_0", "close.echo_payload", "close.reason_utf8_bytes",
    "close.peer_sequence", "close.empty_len", "close.empty_echo", "close.one_byte", "close.return_1_echo",
    "close.ours_echoed_ms", "close.ours_noecho_wait_ms", "close.ours_sequence", "close.ours_long_reason",
    "close.drop_fin_sequence", "close.drop_rst_sequence", "close.drop_reports_client_closed",
    "ping.auto_pong", "ping.auto_pong_latency_ms", "ping.client_sees_server_ping", "ping.pong_payload_echo",
    "ping.rtt_ms", "ping.max_payload_125", "ping.unsolicited_pong_delivered", "ping.silent_no_pong",
    "ping.idle_survives_timeout_secs",
    "rx.frames_split_utf8", "rx.final_flag", "rx.first_flag", "rx.bytes_intact", "rx.chunk_sizes_single_frame",
    "rx.remaining_payload", "rx.binary_flag_on_continuations", "rx.control_inside_fragmented", "rx.order_preserved",
    "rx.invalid_utf8_passthrough", "rx.flow_pause_stops_callbacks", "rx.flow_resume_from_command",
    "rx.flow_resume_form", "rx.flow_backpressure_reached_server",
    "tx.no_fin_frames", "tx.ping_between_fragments", "tx.message_intact", "tx.partial_after_fragment",
    "tx.large_write_result", "tx.large_write_frames", "tx.large_write_intact", "tx.two_writes_one_writeable",
    "tx.autoreply_between_messages", "tx.autoreply_delay_ms", "tx.so_sndbuf_ws", "tx.so_sndbuf_wss",
    "tls.ssl_module_ctx", "tls.load_extra_fired", "tls.insecure_localhost", "tls.insecure_ip_host",
    "tls.insecure_skip_hostname", "tls.version", "tls.untrusted_rejected", "tls.trusted_ca_verified",
    "tls.hostname_with_port", "tls.hostname_ip_literal", "tls.verify_callback_calls", "tls.provided_ctx_connects",
    "tls.provided_ctx_load_extra_fired",
    "coexist.monolithic", "coexist.shared_lws_state", "coexist.lws_symbol_owner", "coexist.both_ws",
    "coexist.both_wss", "coexist.engine_after_probe_teardown", "coexist.engine_lws_log_routed",
    "coexist.context_create_reallocates_ssl_index",
)
# Probe.VaRest.Risky.Lws.StatusInConnectionError.<case>: one run each (WS W0 decision 10); the id is the snake-case name
RISKY_CASES = ("Refused", "ConnectRefused", "DnsFailure", "NoUpgrade", "TlsVerifyFailure", "KilledPending")
# The re-run commands find the host project as Tools/run_tests.sh does: VAREST_HOST, else its default (W0 final
# review M-3); the passport keeps the variable, so the commands work on another checkout too
HOST = os.environ.get("VAREST_HOST", "/w/Unreal/VaRestHost")
LOGS = "${VAREST_HOST:-%s}/Saved/Logs" % HOST
VERDICTS = ("differs", "crashed", "agrees", "recorded", "unverified")
# No closing brace required: a crash can cut the line short, and that must reach json.loads to be counted as malformed
LINE = re.compile(r"LogVaRestLwsProbe: Display: (FACT|PENDING) (\{.*)$")
NO_RETURN = "(the call did not return)"
SECTIONS = (
    ("env", "Environment"),
    ("release", "Not run here: release-pass items"),
    ("svc", "Service thread and commands"),
    ("timeout", "Timeouts"),
    ("hs", "Handshake"),
    ("close", "Close"),
    ("ping", "Ping and pong"),
    ("rx", "Receive"),
    ("tx", "Send"),
    ("tls", "TLS"),
    ("coexist", "Coexistence with the engine's WebSockets module"),
)
COMMANDS_INSIDE = (
    "One service thread per context, waiting up to 10 s in `lws_service` and woken by `lws_cancel_service`; game-thread "
    "commands (connect, write, close, kill, flow control) run inside `LWS_CALLBACK_EVENT_WAIT_CANCELLED` "
    "(`svc.commands_*` compares that with the engine's between-calls placement).")
# Task 4 Step 7 / Task 5 Step 3 switch the rig to the engine's placement when the inside one does not work
COMMANDS_BETWEEN = (
    "One service thread per context, waiting up to 10 s in `lws_service` and woken by `lws_cancel_service`; game-thread "
    "commands (connect, write, close, kill, flow control) are drained between `lws_service` calls, the engine's way: "
    "inside `LWS_CALLBACK_EVENT_WAIT_CANCELLED` they did not work in this run (`svc.commands_inside_wait_cancelled` "
    "differs), so the rig's default was switched.")
# None: the command placement, chosen per run by method()
METHOD = (
    "One `lws_context` per test with WebSocket spec 2.1's shape: one protocol `varest` (`rx_buffer_size` 64 KiB), "
    "`CONTEXT_PORT_NO_LISTEN`, `PEER_CERT_NOT_REQUIRED | DISABLE_OS_CA_CERTS | JUST_USE_RAW_ORIGIN`, 32 KiB header "
    "buffers, `ws_ping_pong_interval = 0`.",
    None,
    "Facts about LWS's own timers (handshake, close-ack and idle timeouts, the blackholes) wait at most 100 ms instead: "
    "LWS checks its deadlines only when `lws_service` returns. Each kill latency is measured with both waits "
    "(`*_short_wait` rows).",
    "Library outcomes (a connection that opened, ended or echoed) are facts, never failed tests; a test fails only when "
    "the harness did not run. Calls that may end the process (`Probe.VaRest.Risky.*`) run one case per process after "
    "a `PENDING` line.",
    "Every callback is recorded in order with its length, flags and a copy of its bytes; the server side comes from "
    "`ws.py`'s journal (`GET /ws/journal`) over HTTP, never through the library under test.",
    "TLS against the test server's committed throwaway CA (`Tools/testserver/certs/`).",
    "The probe never calls `lws_set_log_level` or `lws_set_allocator`.",
)


def read_facts(paths, notes=None):
    """notes (optional dict) gets "malformed": [(file, line number)] and "duplicates": [(id, first file, other file)]"""
    notes = notes if notes is not None else {}
    notes.setdefault("malformed", [])
    notes.setdefault("duplicates", [])
    facts, pending, source = {}, {}, {}
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as stream:
            for number, line in enumerate(stream, 1):
                match = LINE.search(line.rstrip("\r\n"))
                if not match:
                    continue
                try:
                    record = json.loads(match.group(2))
                except ValueError:
                    notes["malformed"].append((os.path.basename(path), number))
                    continue
                key = record.get("id", "") if isinstance(record, dict) else ""
                if not key or key.startswith("rig."):
                    continue
                if match.group(1) == "PENDING":
                    pending.setdefault(key, record)
                elif key in facts:
                    notes["duplicates"].append((key, source[key], os.path.basename(path)))
                else:
                    facts[key] = record
                    source[key] = os.path.basename(path)
    for key, record in pending.items():
        if key not in facts:
            facts[key] = {"id": key, "verdict": "crashed", "test": record.get("test", ""), "spec": NO_RETURN, "evidence": "",
                          "observed": "the process ended inside the probed call (a PENDING line and no FACT)"}
    return facts


def cell(value):
    return str(value).replace("\\", "\\\\").replace("|", "\\|").replace("\r", " ").replace("\n", " ")


def engine_version(engine):
    try:
        with open(os.path.join(engine, "Engine", "Build", "Build.version"), encoding="utf-8") as stream:
            build = json.load(stream)
        return "%d.%d.%d (%s)" % (build["MajorVersion"], build["MinorVersion"], build["PatchVersion"], build.get("BranchName", ""))
    except (OSError, ValueError, KeyError):
        return "unknown"


def cpu_name():
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as stream:
            for line in stream:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def plugin_commit():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    try:
        result = subprocess.run(["git", "-C", root, "rev-parse", "--short", "HEAD"], capture_output=True, text=True, timeout=10)
        sha = result.stdout.strip() or "unknown"
        status = subprocess.run(["git", "-C", root, "status", "--porcelain"], capture_output=True, text=True, timeout=30)
        return sha + " + uncommitted changes" if status.stdout.strip() else sha
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def method(facts):
    """METHOD with the command placement this run used (re-review N-2)"""
    between = facts.get("svc.commands_inside_wait_cancelled", {}).get("verdict") == "differs"
    return [(COMMANDS_BETWEEN if between else COMMANDS_INSIDE) if text is None else text for text in METHOD]


def render(facts, logs, machine, today, missing, notes=None):
    notes = notes or {}
    def observed(key):
        return facts.get(key, {}).get("observed", "not recorded")

    lines = ["# libwebsockets 3.0 probe (%s)" % today, "",
             "WebSocket spec §16.4, milestone W0: libwebsockets as the engine bundles it, driven directly by "
             "`Probe.VaRest.Lws.*` and `Probe.VaRest.Risky.Lws.*` (`Source/VaRestTests/Private/LwsProbe/`) against "
             "`Tools/testserver/ws.py`. One row per fact. **spec** is what the WebSocket spec says, or what it inferred "
             "from libwebsockets 4.3; **verdict** compares it with what 3.0 did: agrees, differs, recorded (a measurement "
             "the spec makes no claim about), unverified (could not run on this host), crashed (the process ended inside "
             "the probed call). The spec column quotes the WebSocket spec as of 2026-10-21, before the W0 corrections; "
             "§0.3 holds the corrected text. Generated by `Tools/lws_probe_passport.py`: re-run it instead of editing "
             "this file.",
             "", "## Machine and build", "", "| | |", "|---|---|"]
    for name, value in machine:
        lines.append("| %s | %s |" % (name, cell(value)))
    lines.append("| libwebsockets | %s |" % cell(observed("env.lws_version")))
    lines.append("| OpenSSL (as VaRestTests' calls resolve) | %s |" % cell(observed("env.openssl_version")))
    lines.append("| Build | %s |" % cell(observed("env.build")))
    lines.append("| Logs | %s |" % cell(", ".join(os.path.basename(path) for path in logs)))
    lines += ["", "## How the probe works", ""] + ["- " + text for text in method(facts)]

    counts = {verdict: 0 for verdict in VERDICTS}
    for record in facts.values():
        counts[record.get("verdict", "recorded")] = counts.get(record.get("verdict", "recorded"), 0) + 1
    lines += ["", "## Summary", "", "| verdict | facts |", "|---|---|"]
    # A verdict outside VERDICTS gets its own row, so the rows always add up to the fact count
    lines += ["| %s | %d |" % (cell(verdict), counts[verdict]) for verdict in list(VERDICTS) + sorted(set(counts) - set(VERDICTS))]
    lines.append("| expected ids missing | %d |" % len(missing))
    if notes.get("malformed"):
        lines += ["", "## Malformed lines", "", "FACT or PENDING lines whose JSON did not parse (a line cut off by a crash), skipped:", ""]
        lines += ["- `%s` line %d" % entry for entry in notes["malformed"]]
    if notes.get("duplicates"):
        lines += ["", "## Duplicate ids", "", "The first record was kept:", ""]
        lines += ["- `%s`: `%s` and `%s`" % entry for entry in notes["duplicates"]]
    if missing:
        lines += ["", "## Missing facts", ""] + ["- `%s`" % key for key in missing]

    for title, verdict in (("Differs from the spec", "differs"), ("Crashed", "crashed")):
        chosen = sorted(key for key, record in facts.items() if record.get("verdict") == verdict)
        lines += ["", "## " + title, ""]
        lines += ["- `%s`: %s (spec: %s)" % (key, cell(facts[key].get("observed", "")), cell(facts[key].get("spec", ""))) for key in chosen] or ["none"]

    lines += ["", "## Results"]
    known = {prefix for prefix, _ in SECTIONS}
    other = sorted(key for key in facts if key.split(".", 1)[0] not in known)
    for prefix, title in SECTIONS + ((None, "Other"),) * bool(other):
        if prefix is None:
            lines += ["", "### " + title, "", "| id | verdict | observed | spec | evidence |", "|---|---|---|---|---|"]
            lines += ["| `%s` | %s | %s | %s | %s |" % (key, facts[key].get("verdict", ""), cell(facts[key].get("observed", "")),
                                                        cell(facts[key].get("spec", "")), cell(facts[key].get("evidence", "")))
                      for key in other]
            continue
        keys = [key for key in EXPECTED if key.split(".", 1)[0] == prefix and key in facts]
        keys += sorted(key for key in facts if key.split(".", 1)[0] == prefix and key not in EXPECTED)
        lines += ["", "### " + title, "", "| id | verdict | observed | spec | evidence |", "|---|---|---|---|---|"]
        for key in keys:
            record = facts[key]
            lines.append("| `%s` | %s | %s | %s | %s |" % (key, record.get("verdict", ""), cell(record.get("observed", "")),
                                                           cell(record.get("spec", "")), cell(record.get("evidence", ""))))
    lines += ["", "## Re-run", "", "```",
              "Tools/run_tests.sh Probe.VaRest.Lws; cp %s/VaRestTests.log %s/VaRestProbe-main.log" % (LOGS, LOGS),
              "for CASE in %s; do" % " ".join(RISKY_CASES),
              "\tTools/run_tests.sh --no-build \"Probe.VaRest.Risky.Lws.StatusInConnectionError.$CASE\"",
              "\tcp %s/VaRestTests.log \"%s/VaRestProbe-risky-$CASE.log\"" % (LOGS, LOGS),
              "done",
              "python3 -I Tools/lws_probe_passport.py --log %s/VaRestProbe-main.log %s/VaRestProbe-risky-*.log \\" % (LOGS, LOGS),
              "\t--out docs/passport/$(date +%F)-lws30-probe.md",
              "```", ""]
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description="libwebsockets 3.0 probe passport")
    parser.add_argument("--log", action="extend", nargs="+", default=[], help="run logs with FACT lines (one or more; repeatable)")
    parser.add_argument("--out", help="the passport to write")
    parser.add_argument("--engine", default=os.environ.get("VAREST_ENGINE", "/w/Unreal/UnrealEngine"))
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    if args.selftest:
        return 0 if unittest.main(module=__name__, argv=[sys.argv[0]], exit=False).result.wasSuccessful() else 1
    if not args.log or not args.out:
        parser.error("--log and --out are required")
    notes = {}
    facts = read_facts(args.log, notes)
    missing = [key for key in EXPECTED if key not in facts]
    machine = [("Date", datetime.date.today().isoformat()), ("OS", platform.platform()), ("CPU", cpu_name()),
               ("Engine", engine_version(args.engine)), ("Plugin commit", plugin_commit())]
    text = render(facts, args.log, machine, datetime.date.today().isoformat(), missing, notes)
    temporary = args.out + ".tmp"
    with open(temporary, "w", encoding="utf-8") as stream:
        stream.write(text)
    os.replace(temporary, args.out)
    unknown = sorted(set(facts) - set(EXPECTED))
    if unknown:
        print("facts not in EXPECTED (kept in the passport): " + ", ".join(unknown))
    for entry in notes["duplicates"]:
        print("duplicate id %s: %s and %s (kept the first)" % entry, file=sys.stderr)
    if notes["malformed"]:
        print("malformed lines skipped: " + ", ".join("%s:%d" % entry for entry in notes["malformed"]), file=sys.stderr)
    if missing:
        print("missing facts: " + ", ".join(missing), file=sys.stderr)
        return 1
    print("passport: %d facts -> %s" % (len(facts), args.out))
    return 0


class SelfTest(unittest.TestCase):
    def write_log(self, directory, lines):
        path = os.path.join(directory, "run.log")
        with open(path, "w", encoding="utf-8") as stream:
            stream.write("\n".join(lines) + "\n")
        return path

    def fact(self, key, verdict="agrees", observed="ok"):
        return "[2026.10.21-12.00.00:000][  0]LogVaRestLwsProbe: Display: FACT " + json.dumps(
            {"id": key, "verdict": verdict, "observed": observed, "spec": "s", "evidence": "e", "test": "t"})

    def test_facts_pending_and_rig_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            log = self.write_log(directory, [
                self.fact("hs.raw_origin", "differs", "Origin: http://x | y"),
                "[...]LogVaRestLwsProbe: Display: PENDING " + json.dumps({"id": "hs.status_in_connection_error.no_upgrade"}),
                self.fact("rig.fact_line"),
                "LogSomethingElse: Display: FACT {not ours}",
            ])
            facts = read_facts([log])
        self.assertEqual(sorted(facts), ["hs.raw_origin", "hs.status_in_connection_error.no_upgrade"])
        self.assertEqual(facts["hs.status_in_connection_error.no_upgrade"]["verdict"], "crashed")
        text = render(facts, [log], [("Date", "2026-10-21")], "2026-10-21", ["env.lws_version"])
        self.assertIn("| `hs.raw_origin` | differs | Origin: http://x \\| y |", text)
        self.assertIn("## Missing facts", text)
        self.assertIn("- `hs.status_in_connection_error.no_upgrade`: the process ended inside the probed call", text)

    def test_a_pending_line_with_its_fact_is_not_a_crash(self):
        with tempfile.TemporaryDirectory() as directory:
            log = self.write_log(directory, [
                "[...]LogVaRestLwsProbe: Display: PENDING " + json.dumps({"id": "hs.status_in_connection_error.refused"}),
                self.fact("hs.status_in_connection_error.refused", "differs", "401 -> 0"),
            ])
            facts = read_facts([log])
        self.assertEqual(facts["hs.status_in_connection_error.refused"]["verdict"], "differs")

    def test_missing_ids_fail_the_run(self):
        with tempfile.TemporaryDirectory() as directory:
            log = self.write_log(directory, [self.fact("env.lws_version")])
            out = os.path.join(directory, "passport.md")
            self.assertEqual(main(["--log", log, "--out", out]), 1)
            with open(out, encoding="utf-8") as stream:
                self.assertIn("`svc.cancel_wake_ms`", stream.read())
            # One --log with several files (the per-case risky logs): every id from either file counts
            first = os.path.join(directory, "first.log")
            second = os.path.join(directory, "second.log")
            half = len(EXPECTED) // 2
            with open(first, "w", encoding="utf-8") as stream:
                stream.write("\n".join(self.fact(key) for key in EXPECTED[:half]) + "\n")
            with open(second, "w", encoding="utf-8") as stream:
                stream.write("\n".join(self.fact(key) for key in EXPECTED[half:]) + "\n")
            self.assertEqual(main(["--log", first, second, "--out", out]), 0)

    def test_expected_ids_are_unique_and_sectioned(self):
        self.assertEqual(len(EXPECTED), len(set(EXPECTED)))
        prefixes = {prefix for prefix, _ in SECTIONS}
        self.assertEqual(sorted({key.split(".", 1)[0] for key in EXPECTED} - prefixes), [])

    def test_method_follows_the_command_placement(self):
        inside = render({}, [], [], "2026-10-21", [])
        self.assertIn("run inside `LWS_CALLBACK_EVENT_WAIT_CANCELLED`", inside)
        flipped = {"svc.commands_inside_wait_cancelled": {"verdict": "differs", "observed": "established=no"}}
        between = render(flipped, [], [], "2026-10-21", [])
        self.assertIn("drained between `lws_service` calls", between)
        self.assertNotIn("run inside `LWS_CALLBACK_EVENT_WAIT_CANCELLED`", between)

    def test_malformed_line_is_noted_not_fatal(self):
        with tempfile.TemporaryDirectory() as directory:
            log = self.write_log(directory, [
                "[...]LogVaRestLwsProbe: Display: PENDING " + json.dumps({"id": "hs.status_in_connection_error.refused"}),
                '[...]LogVaRestLwsProbe: Display: FACT {"id": "hs.status_in_connection_error.refused", "verd',
                self.fact("hs.raw_origin"),
            ])
            notes = {}
            facts = read_facts([log], notes)
            text = render(facts, [log], [], "2026-10-21", [], notes)
        self.assertEqual(notes["malformed"], [("run.log", 2)])
        self.assertEqual(facts["hs.status_in_connection_error.refused"]["verdict"], "crashed")
        self.assertEqual(facts["hs.status_in_connection_error.refused"]["spec"], NO_RETURN)
        self.assertIn("## Malformed lines", text)
        self.assertIn("`run.log` line 2", text)

    def test_duplicate_ids_keep_the_first_and_are_listed(self):
        with tempfile.TemporaryDirectory() as directory:
            first = os.path.join(directory, "a.log")
            second = os.path.join(directory, "b.log")
            for path, observed in ((first, "one"), (second, "two")):
                with open(path, "w", encoding="utf-8") as stream:
                    stream.write(self.fact("hs.raw_origin", "agrees", observed) + "\n")
            notes = {}
            facts = read_facts([first, second], notes)
            text = render(facts, [first, second], [], "2026-10-21", [], notes)
        self.assertEqual(facts["hs.raw_origin"]["observed"], "one")
        self.assertEqual(notes["duplicates"], [("hs.raw_origin", "a.log", "b.log")])
        self.assertIn("- `hs.raw_origin`: `a.log` and `b.log`", text)

    def test_unknown_verdicts_and_prefixes_are_shown(self):
        facts = {"hs.x": {"verdict": "weird", "observed": "o"}, "zzz.y": {"verdict": "agrees", "observed": "o2"}}
        text = render(facts, [], [], "2026-10-21", [])
        self.assertIn("| weird | 1 |", text)
        self.assertIn("### Other", text)
        self.assertIn("| `zzz.y` | agrees | o2 |", text)

    def test_every_risky_case_has_its_id(self):
        ids = ["hs.status_in_connection_error." + re.sub(r"(?<!^)(?=[A-Z])", "_", case).lower() for case in RISKY_CASES]
        self.assertEqual([key for key in ids if key not in EXPECTED], [])
        self.assertEqual(sorted(key for key in EXPECTED if key.startswith("hs.status_in_connection_error.")), sorted(ids))
        text = render({}, [], [], "2026-10-21", [])
        self.assertIn("Probe.VaRest.Risky.Lws.StatusInConnectionError.$CASE", text)


if __name__ == "__main__":
    sys.dont_write_bytecode = True
    sys.exit(main())
