#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
import argparse
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import math
import pathlib
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('b1_controller', HERE / 'run_lws_proxy_probe.py')
controller = importlib.util.module_from_spec(spec)
spec.loader.exec_module(controller)
EXPECTED = tuple(row[0] for row in controller.CASES)


def check_cases(ids):
    return [] if (isinstance(ids, list) and all(isinstance(i, str) for i in ids)
                  and len(ids) == len(set(ids)) and set(ids) == set(EXPECTED)) else ['incomplete/duplicate matrix']


def fields(value, label, types):
    """Validate JSON scalars before comparing them or calling their methods."""
    if not isinstance(value, dict):
        return [label + ' root must be an object']
    errors = []
    for name, kind in types.items():
        item = value.get(name)
        valid = ((type(item) is int and -sys.float_info.max <= item <= sys.float_info.max
                  or type(item) is float and math.isfinite(item))
                 if kind == 'number' else type(item) is kind)
        if not valid:
            errors.append(label + ' invalid field: ' + name)
    return errors


def trace_shape(trace):
    errors = fields(trace, 'trace', dict(case=str, mode=str, connection=int, schema=int,
        dropped_events=int, primary_completed=bool, opened=bool, echoed=bool, destroyed=bool,
        destroyed_before_shutdown=bool, shutdown_completed=bool, configured_proxy_absent=bool,
        explicit_proxy=str, tls=bool, untrusted=bool, redirect_hostname_skip=bool,
        primary_seconds='number', primary_budget_seconds='number', events=list))
    if errors:
        return errors
    if trace['primary_seconds'] < 0 or trace['primary_budget_seconds'] <= 0:
        errors.append('invalid observation duration/budget')
    if 'destroy_wait_completed' in trace and type(trace['destroy_wait_completed']) is not bool:
        errors.append('trace invalid field: destroy_wait_completed')
    for event in trace['events']:
        found = fields(event, 'trace event', dict(seq=int, conn=int, wsi=int, reason=int,
            name=str, note=str, time='number', len=int, input_present=bool, hex=str,
            depth=int, preverify=int, x509_error=int))
        errors.extend(found)
        if found:
            continue
        if event['wsi'] < 0 or event['conn'] < 0 or event['len'] < 0:
            errors.append('negative trace identity/length')
        try:
            bytes.fromhex(event['hex'])
        except ValueError:
            errors.append('malformed trace hex')
    return errors


def journal_shape(journal, label, proxy=False):
    errors = fields(journal, label + ' journal', dict(events=list, dropped=int,
                    **(dict(mode=str, lifetime_seconds='number', active=int) if proxy
                       else dict(connections=int, ended=bool))))
    if errors:
        return errors
    if not proxy:
        errors.extend(controller.check_journal(journal, label))
    for event in journal['events']:
        found = fields(event, label + ' event', dict(ev=str))
        errors.extend(found)
        if found:
            continue
        if proxy:
            errors.extend(fields(event, label + ' event', dict(id=int)))
        if event['ev'] in ('connect', 'request'):
            headers = event.get('headers')
            if (not isinstance(headers, list) or any(not isinstance(h, list) or len(h) != 2
                    or any(not isinstance(part, str) for part in h) for h in headers)):
                errors.append(label + ' event invalid headers')
        if event['ev'] == 'connect':
            errors.extend(fields(event, label + ' connect', dict(authority=str)))
        if event['ev'] in ('denied', 'response'):
            errors.extend(fields(event, label + ' response', dict(status=int)))
        if event['ev'] == 'silent':
            errors.extend(fields(event, label + ' silent', dict(lifetime_seconds='number')))
    return errors


def events_of(journal, kind):
    return [event for event in journal.get('events', []) if event.get('ev') == kind]


def lifecycle(trace, *, allow_silent_destroy_only=False):
    errors = trace_shape(trace)
    if errors:
        return dict(errors=errors)
    connection = trace.get('connection')
    if not isinstance(connection, int) or connection <= 0:
        errors.append('invalid attempt connection identity')
    events = [e for e in trace['events'] if e.get('conn') == connection]
    def marker(label):
        found = [e for e in events if e['reason'] == -7 and e.get('note') == label]
        if len(found) != 1:
            errors.append('missing/duplicate lifecycle marker: ' + label)
            return dict(seq=0, len=-1)
        return found[0]
    primary = marker('proxy-primary-finished')
    shutdown = marker('proxy-shutdown-begin')
    stopped = marker('proxy-shutdown-end')
    if not 0 < primary['seq'] < shutdown['seq'] < stopped['seq']:
        errors.append('lifecycle marker order')
    opens = [e for e in events if e['reason'] == 3]
    terminals = [e for e in events if e['reason'] in (1, 4, 45, 75)]
    certs = [e for e in events if e['reason'] == 58]
    # Identity can resolve after CREATE. Keep that actual raw record; never invent
    # a connection association or count an internal conn=0-only token as an attempt.
    tokens = {e['wsi'] for e in events if e['wsi'] > 0}
    token_events = [e for e in trace['events'] if e['wsi'] in tokens]
    creates = [e for e in token_events if e['reason'] == 29]
    destroys = [e for e in token_events if e['reason'] == 30]
    token_lifecycles = []
    for token in sorted(tokens):
        made = [e['seq'] for e in creates if e['wsi'] == token]
        ended = [e['seq'] for e in destroys if e['wsi'] == token]
        if not made:
            errors.append('attempt token missing create: ' + str(token))
        if not ended:
            errors.append('attempt token missing destruction: ' + str(token))
        elif len(ended) != 1:
            errors.append('attempt token duplicate destruction: ' + str(token))
        if made and ended and max(made) >= min(ended):
            errors.append('attempt token create after destruction: ' + str(token))
        if any(e['conn'] not in (0, connection) for e in token_events if e['wsi'] == token):
            errors.append('attempt token has conflicting connection identity: ' + str(token))
        token_lifecycles.append(dict(wsi=token, create_sequences=made, destroy_sequences=ended))
    for event in trace['events']:
        if event['conn'] != connection and event['wsi'] not in tokens:
            continue
        if event['reason'] in (1, 3, 4, 8, 24, 30, 45, 58, 75):
            if event.get('wsi', 0) <= 0 or not any(
                    c['wsi'] == event['wsi'] and c['seq'] < event['seq'] for c in creates):
                errors.append('callback lacks associated create/token')
            if any(d['wsi'] == event.get('wsi') and d['seq'] < event['seq'] for d in destroys):
                errors.append('callback follows token destruction')
    observed_primary = any(e['reason'] in (1, 3, 30) and e['seq'] < primary['seq'] for e in events)
    observed_destroyed = bool(tokens) and all(
        len(t['destroy_sequences']) == 1 and t['destroy_sequences'][0] < stopped['seq']
        for t in token_lifecycles)
    observed_before = observed_destroyed and all(d['seq'] < shutdown['seq'] for d in destroys)
    observed_echo = any(e['reason'] == 8 and bytes.fromhex(e.get('hex', '')) == b'b1-echo' for e in events)
    derived = dict(primary_completed=observed_primary, opened=bool(opens), echoed=observed_echo,
                   destroyed=observed_destroyed, destroyed_before_shutdown=observed_before,
                   shutdown_completed=stopped['seq'] > shutdown['seq'] > 0)
    for name, observed in derived.items():
        if not isinstance(trace.get(name), bool) or trace[name] != observed:
            errors.append('contradictory lifecycle summary: ' + name)
    if primary['len'] not in (0, 1) or bool(primary['len']) != observed_primary:
        errors.append('primary completion marker contradicts callbacks')
    if not observed_primary or not observed_destroyed:
        errors.append('missing primary/destruction callback')
    # Root's measured ruling: only immutable silent rows may end with DESTROY
    # alone. It must precede both primary completion and shutdown, for every
    # associated token. Fixture lifetime/EOF checks remain mandatory below.
    silent_destroy_only = (allow_silent_destroy_only and not terminals and observed_destroyed
                           and observed_primary and all(d['seq'] < primary['seq'] < shutdown['seq']
                                                       for d in destroys))
    if not silent_destroy_only and not any(
            t.get('wsi', 0) > 0 and any(d['wsi'] == t['wsi'] and t['seq'] < d['seq'] for d in destroys)
            for t in terminals):
        errors.append('missing terminal callback before matching destruction')
    return dict(errors=errors, events=events, opens=opens, certs=certs, terminals=terminals,
                destroys=destroys, shutdown_seq=shutdown['seq'], derived=derived,
                token_lifecycles=token_lifecycles, silent_destroy_only=silent_destroy_only)


def fixture_witness(journal, connect, witness, status=None):
    # Fixture IDs are local to this listener, not client connection IDs.
    flow = [e for e in journal['events'] if e.get('id') == connect.get('id')]
    starts = [i for i, e in enumerate(flow) if e['ev'] == 'connect']
    seen = [i for i, e in enumerate(flow) if e['ev'] == witness and (status is None or e.get('status') == status)]
    ends = [i for i, e in enumerate(flow) if e['ev'] == 'end']
    return bool(len(starts) == 1 and len(seen) == 1 and len(ends) == 1
                and starts[0] < seen[0] < ends[0])


def summarize(case, trace, a, b, source, target, *, expected_case_id=None):
    shape_errors = fields(case, 'case', dict(case=str, mode=str, tls=bool, untrusted=bool,
        redirect_hostname_skip=bool, target_port=int, proxy_a=str, proxy_b=str,
        group_drained=bool, harness_errors=list, runner_rc=int))
    if not shape_errors and any(not isinstance(e, str) for e in case['harness_errors']):
        shape_errors.append('case harness_errors must contain strings')
    shape_errors.extend(trace_shape(trace))
    for label, journal, proxy in (('proxy-a', a, True), ('proxy-b', b, True),
                                   ('source', source, False), ('target', target, False)):
        shape_errors.extend(journal_shape(journal, label, proxy))
    if shape_errors:
        return dict(case=expected_case_id, harness_errors=shape_errors, control_errors=[])
    bound_id = case['case'] if expected_case_id is None else expected_case_id
    if bound_id not in EXPECTED:
        return dict(case=bound_id, harness_errors=[], control_errors=['unknown immutable case'])
    expected = controller.case_definition(bound_id)
    mode = expected['mode']
    harness_errors = list(case.get('harness_errors', []))
    if case['runner_rc'] != 0:
        harness_errors.append('editor/automation runner failed')
    harness_errors.extend(controller.check_trace(trace, case['case']))
    if not case.get('group_drained'):
        harness_errors.append('owned editor process group not drained')
    errors = []
    if case.get('case') != bound_id or trace.get('case') != bound_id:
        errors.append('immutable case directory identity')
    for field in ('mode', 'tls', 'untrusted', 'redirect_hostname_skip'):
        if case.get(field) != expected[field] or trace.get(field) != expected[field]:
            errors.append('immutable case binding: ' + field)
    if trace.get('explicit_proxy') != (case.get('proxy_a') if expected['explicit_proxy'] else ''):
        errors.append('immutable explicit proxy option')
    if mode in ('env', 'precedence', 'env-noproxy') and trace.get('configured_proxy_absent') is not True:
        errors.append('engine configured proxy not absent')
    if trace.get('primary_budget_seconds') != expected['primary_budget_seconds']:
        errors.append('immutable primary observation budget')
    if a.get('mode') != expected['fixture_mode'] or b.get('mode') != 'tunnel':
        errors.append('immutable fixture mode')
    if a.get('lifetime_seconds') != expected['fixture_lifetime_seconds'] or b.get('lifetime_seconds') != 12.0:
        errors.append('immutable fixture lifetime')
    for journal in (a, b):
        if journal.get('active') != 0:
            harness_errors.append('fixture handlers not drained')
    life = lifecycle(trace, allow_silent_destroy_only=mode == 'silent')
    errors.extend(life['errors'])
    a_connect = events_of(a, 'connect')
    b_connect = events_of(b, 'connect')
    connects = a_connect + b_connect
    requests = events_of(source, 'request')
    targets = events_of(target, 'request')
    for journal in (a, b, source, target):
        if journal.get('dropped', 0):
            errors.append('journal loss')
    for journal in (source, target):
        if journal.get('connections') != len(events_of(journal, 'request')):
            errors.append('target journal request count contradicts connections')
        if journal.get('connections', 0) and journal.get('ended') is not True:
            errors.append('target journal did not end')
    for connect in connects:
        if not mode.startswith('redirect') and connect['authority'] not in (
                '127.0.0.1:%d' % case['target_port'], 'localhost:%d' % case['target_port']):
            errors.append('CONNECT authority differs from requested target')
        if any(name.lower() == 'authorization' for name, _ in connect['headers']):
            errors.append('target Authorization present in CONNECT')
    positive = mode in ('direct', 'explicit', 'env', 'precedence', 'env-noproxy')
    if positive:
        if not life['derived']['opened'] or not life['derived']['echoed'] or len(requests) != 1:
            errors.append('positive open/echo/Upgrade control failed')
        if requests and not any(n.lower() == 'authorization' and v == 'Bearer b1-probe-marker'
                                for n, v in requests[0]['headers']):
            errors.append('positive synthetic-header control failed')
        if expected['tls']:
            trusted = [c for c in life['certs'] if c.get('preverify') == 1 and c.get('x509_error') == 0
                       and isinstance(c.get('depth'), int) and c['depth'] >= 0
                       and c.get('wsi', 0) > 0 and any(o['wsi'] == c['wsi'] and c['seq'] < o['seq']
                                                      for o in life['opens'])]
            if not trusted or any(c.get('preverify') != 1 or c.get('x509_error') != 0 for c in life['certs']):
                errors.append('trusted wss lacks associated successful certificate evidence')
    if mode == 'direct' and connects:
        errors.append('direct control used proxy')
    if mode == 'explicit' and (len(a_connect) != 1 or b_connect):
        errors.append('explicit control did not use one A tunnel')
    if mode in ('deny', 'close', 'silent'):
        if len(a_connect) != 1 or b_connect or requests or targets or life['derived']['opened']:
            errors.append('negative proxy control reached wrong stage')
        if any(e['ev'] in ('invalid', 'io_error', 'hold_deadline', 'relay_deadline', 'stopped', 'upstream', 'tunnel')
               for e in a['events']):
            errors.append('proxy stimulus substituted by IO/deadline/stop/upstream')
        witness = {'deny': 'denied', 'close': 'closed_without_response', 'silent': 'silent'}[mode]
        if len(a_connect) != 1 or not fixture_witness(a, a_connect[0], witness, 403 if mode == 'deny' else None):
            errors.append('missing successful fixture witness: ' + witness)
        if not life['silent_destroy_only'] and not any(t['seq'] < life['shutdown_seq'] and any(
                d['wsi'] == t['wsi'] and t['seq'] < d['seq'] < life['shutdown_seq'] for d in life['destroys'])
                   for t in life['terminals']):
            errors.append('negative control did not end before context shutdown')
        if mode == 'silent':
            if len(a_connect) != 1 or not fixture_witness(a, a_connect[0], 'peer_eof'):
                errors.append('silent fixture lacks client EOF before teardown')
            flow = [e for e in a['events'] if a_connect and e.get('id') == a_connect[0].get('id')]
            silent_positions = [i for i, e in enumerate(flow) if e['ev'] == 'silent' and e.get('lifetime_seconds') == 40.0]
            eof_positions = [i for i, e in enumerate(flow) if e['ev'] == 'peer_eof']
            if len(silent_positions) != 1 or len(eof_positions) != 1 or silent_positions[0] >= eof_positions[0]:
                errors.append('silent lifetime/EOF witness order')
            # No clock comparison between Python and UE: callbacks precede UE shutdown, and
            # the fixture ends only on client EOF with neither its deadline nor stop firing.
            if not life['derived']['destroyed_before_shutdown']:
                errors.append('silent attempt required forced context destruction')
    # A tunnel witness must follow CONNECT and upstream connect and precede fixture end.
    for journal, connections in ((a, a_connect), (b, b_connect)):
        if journal.get('mode') == 'tunnel':
            for connect in connections:
                if not fixture_witness(journal, connect, 'upstream') or not fixture_witness(journal, connect, 'tunnel'):
                    errors.append('missing successful tunnel/upstream witness')
                flow = [e for e in journal['events'] if e.get('id') == connect.get('id')]
                tunnel_index = next((i for i, e in enumerate(flow) if e['ev'] == 'tunnel'), len(flow))
                upstream_index = next((i for i, e in enumerate(flow) if e['ev'] == 'upstream'), len(flow))
                if upstream_index >= tunnel_index:
                    errors.append('tunnel witness precedes upstream connection')
                if any(e['ev'] in ('io_error', 'invalid', 'stopped', 'relay_deadline') for e in flow[:tunnel_index]):
                    errors.append('tunnel stimulus substituted by fixture failure')
    if mode.startswith('redirect'):
        if not any(e.get('status') == 302 for e in events_of(source, 'response')):
            errors.append('source redirect response unobserved')
    if mode in ('untrusted', 'upgrade403'):
        if len(a_connect) != 1 or not events_of(a, 'tunnel') or life['derived']['opened']:
            errors.append('post-tunnel negative control invalid')
        if any(e['ev'] in ('hold_deadline', 'relay_deadline', 'stopped') for e in a['events']):
            errors.append('post-tunnel control substituted by fixture deadline/stop')
        if mode == 'upgrade403' and not any(e.get('status') == 403 for e in events_of(source, 'response')):
            errors.append('target upgrade refusal unobserved')
        if mode == 'untrusted':
            if requests:
                errors.append('untrusted target received upgrade')
            failed = [c for c in life['certs'] if c.get('preverify') == 0 and c.get('x509_error', 0) > 0
                      and isinstance(c.get('depth'), int) and c['depth'] >= 0
                      and c.get('wsi', 0) > 0 and any(t['wsi'] == c['wsi'] and c['seq'] < t['seq']
                          and any(d['wsi'] == c['wsi'] and t['seq'] < d['seq'] < life['shutdown_seq']
                                  for d in life['destroys']) for t in life['terminals'])
                      and not any(t['wsi'] == c['wsi'] and t['seq'] < c['seq'] for t in life['terminals'])]
            if not failed:
                errors.append('untrusted wss lacks associated failed certificate before terminal/destruction')
            # A relay close/reset after genuine cert rejection can be teardown. It cannot
            # replace this required client certificate witness; no-cert EOF/IO always fails.
    event_rows = trace['events']
    callback_errors = [dict(seq=e['seq'], conn=e['conn'], wsi=e['wsi'],
                            input_present=e['input_present'], length=e['len'], hex=e['hex'])
                       for e in event_rows if e['reason'] == 1]
    certs = [dict(seq=e['seq'], conn=e['conn'], wsi=e['wsi'], depth=e['depth'],
                  preverify=e['preverify'], x509_error=e['x509_error'])
             for e in event_rows if e['reason'] == 58]
    counts = {name: sum(e['reason'] == reason and e['conn'] == trace['connection'] for e in event_rows)
              for name, reason in (('append', 24), ('create', 29), ('destroy', 30), ('cert', 58))}
    routing = 'A+B' if a_connect and b_connect else 'A' if a_connect else 'B' if b_connect else 'direct'
    ordered = [dict(seq=e['seq'], name=e['name'], reason=e['reason'], conn=e['conn'], wsi=e['wsi'], note=e['note'])
               for e in event_rows]
    return dict(case=case['case'], opened=life['derived']['opened'], echoed=life['derived']['echoed'],
                route=routing, connect_a=len(a_connect), connect_b=len(b_connect),
                counts=counts, certificates=certs, errors=callback_errors, ordered=ordered,
                primary_seconds=trace['primary_seconds'], primary_budget_seconds=trace['primary_budget_seconds'],
                destroyed=life['derived']['destroyed'], destroyed_before_shutdown=life['derived']['destroyed_before_shutdown'],
                source_requests=len(requests), target_requests=len(targets),
                redirect_hostname_skip=trace['redirect_hostname_skip'],
                fixture_witnesses=[e for e in a['events'] if e['ev'] in (
                    'denied', 'closed_without_response', 'silent', 'peer_eof', 'hold_deadline', 'stopped', 'io_error', 'end')],
                source_authorization=[[v for n, v in e['headers'] if n.lower() == 'authorization'] for e in requests],
                target_authorization=[[v for n, v in e['headers'] if n.lower() == 'authorization'] for e in targets],
                token_lifecycles=life['token_lifecycles'],
                silent_destroy_only=life['silent_destroy_only'],
                destroy_wait_completed=trace.get('destroy_wait_completed'),
                harness_errors=harness_errors, control_errors=errors)

def render(manifest, rows, run):
    lines = ['# libwebsockets 3.0 — B1 proxy passport', '',
             '<!-- Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved. -->', '',
             'Generated from a complete raw-rig matrix; values below are observed, not production policy.', '',
             'Source commit: `' + manifest['commit'] + '`. Scope: ' + manifest['scope'] + '.',
             'Evidence directory: `' + str(run) + '`; `sha256.json` hashes every original artifact.',
             'Working-tree state at run start: `' + ('dirty (see manifest)' if manifest['dirty'] else 'clean') + '`.', '',
             '## Results', '',
             '| Case | Open/echo | Route | APPEND/create/destroy/cert | Source/redirect requests | Seconds |',
             '|---|---|---|---|---|---|']
    for row in rows:
        counts = row['counts']
        lines.append('| %s | %s/%s | %s | %s/%s/%s/%s | %s/%s | %.6f |' % (
            row['case'], row['opened'], row['echoed'], row['route'], counts['append'], counts['create'],
            counts['destroy'], counts['cert'], row['source_requests'], row['target_requests'], row['primary_seconds']))
    lines += ['', '## Exact callback and journal observations', '',
              'Each ordered list includes context/unresolved callbacks (conn=0), pseudo-events and repeated callbacks.',
              'Counts use the recorded attempt connection ID. Token lifecycles retain earlier conn=0 CREATE records',
              'when that token later becomes associated; every distinct attempt token must have one recorded destruction.',
              'Certificate callbacks are per verification operation/depth. A count is not a connection count.',
              'Error hex is bounded to 256 bytes; length is raw callback len, and pointer presence is independent.',
              'Silent rows require native destruction before UE shutdown, with client EOF and no fixture deadline/stop.',
              'Root accepted destruction-only silent completion: all tokens must be destroyed before the primary marker',
              'and shutdown. Zero error callbacks stays zero; no separate terminal callback is invented.',
              'Other negative controls retain the separate terminal-before-destruction requirement.',
              'Case mode/TLS/hostname flags are checked against the fixed matrix, and TLS controls require associated certificates.',
              'Redirect rows record the initial hostname-skip option for numeric loopback controls; chain checks remain.',
              'That flag does not establish its effectiveness after a library redirect/reset; inspect the recorded certificate results.',
              'Synthetic Authorization is appended on every raw APPEND; target presence is not proof of automatic replay.', '']
    for row in rows:
        lines += ['### ' + row['case'], '', '```json', json.dumps(row, indent=2, sort_keys=True), '```', '']
    lines += ['## B2 decision inputs', '',
              'Compare direct/explicit ws and wss ordered APPEND, WSI_CREATE and certificate positions before changing any veto.',
              'Compare denial, abrupt close and silence error pointer/length/bytes and terminal/destruction order.',
              'Compare post-tunnel untrusted TLS and target HTTP403 before choosing any contextual empty-error grouping.',
              'Environment rows identify which listener saw CONNECT; no_proxy=* rows are observations, not a promised bypass API.',
              'A denial fixture status is ground truth on the proxy side, not a target HTTP status and not evidence of callback text.',
              'If these public signals coincide, they do not distinguish the cases. A broader proxied pre-upgrade empty-error',
              'ConnectFailed grouping requires an explicit root B2 ruling and preservation of TLS/redirect/cancel/deadline failures.',
              'No production classifier or redirect-veto change was made by B1. Raw redirects were allowed for measurement.',
              'B2 must prove successful production ws/wss proxying and no redirected target request or credential forwarding.', '',
              '## Remaining release gates', '',
              'Native Windows, macOS, Android/iOS and cooked/shared-copy runtime measurements were not run by this Linux editor matrix.',
              'The independent W1a audit completed NoPCH link/runtime validation (572/0) and normal restoration (Module 1/0);',
              'hqx.2 was closed by root. Those attributed results are not B1 execution evidence. Future build guards remain.',
              'VaRest-rti residual and the WebSocket parent epic remain outside B1 completion.', '']
    return '\n'.join(lines)


def read_evidence(path, problems):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError) as error:
        problems.append('invalid evidence ' + str(path) + ': ' + str(error))
        return None


def generate(run, out):
    problems = []
    manifest = read_evidence(run / 'manifest.json', problems)
    manifest_errors = fields(manifest, 'manifest', dict(schema=int, commit=str, scope=str,
        dirty=str, acquisition_only=bool, cases=list, results=list))
    if not manifest_errors:
        manifest_errors.extend(check_cases(manifest['cases']))
        if manifest['schema'] != 1 or manifest['acquisition_only'] is not True:
            manifest_errors.append('manifest identity/schema')
        for result in manifest['results']:
            manifest_errors.extend(fields(result, 'manifest result', dict(case=str, harness_ok=bool)))
        if not manifest_errors and ([r['case'] for r in manifest['results']] != manifest['cases']
                or any(not r['harness_ok'] for r in manifest['results'])):
            manifest_errors.append('manifest result set incomplete, reordered or failed')
    problems.extend(manifest_errors)
    hashes = read_evidence(run / 'sha256.json', problems)
    if not isinstance(hashes, dict):
        problems.append('artifact hashes root must be an object')
        hashes = {}
    required = {'manifest.json', 'server.log'}
    for case_id in EXPECTED:
        required.update(case_id + '/' + name for name in ('case.json', 'trace.json', 'editor.log', 'runner.txt',
                                                         'proxy-a.json', 'proxy-b.json', 'source.json', 'target.json'))
    if not required.issubset(hashes):
        problems.append('artifact manifest missing required files')
    for name, expected_hash in hashes.items():
        if (not isinstance(name, str) or not isinstance(expected_hash, str)
                or len(expected_hash) != 64 or any(c not in '0123456789abcdef' for c in expected_hash)):
            problems.append('invalid artifact hash entry: ' + str(name))
            continue
        path = (run / name).resolve()
        if not path.is_relative_to(run.resolve()) or not path.is_file():
            problems.append('missing/outside artifact: ' + name)
        elif hashlib.sha256(path.read_bytes()).hexdigest() != expected_hash:
            problems.append('artifact hash mismatch: ' + name)
    rows = []
    # Fixed names only: malformed/untrusted manifest paths cannot select evidence.
    for case_id in EXPECTED:
        directory = run / case_id
        values = [read_evidence(directory / (name + '.json'), problems)
                  for name in ('case', 'trace', 'proxy-a', 'proxy-b', 'source', 'target')]
        row = summarize(*values, expected_case_id=case_id)
        rows.append(row)
        problems.extend(case_id + ': harness: ' + error for error in row['harness_errors'])
        problems.extend(case_id + ': control: ' + error for error in row['control_errors'])
    if problems:
        print('\n'.join(problems), file=sys.stderr)
        return 1  # do not replace a prior passport with incomplete evidence
    # Finish rendering before touching a prior passport, then replace atomically.
    content = render(manifest, rows, run)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=out.parent,
                                         prefix=out.name + '.', delete=False) as stream:
            temporary = pathlib.Path(stream.name)
            stream.write(content)
        temporary.replace(out)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()
    return 0


class PassportTests(unittest.TestCase):
    # All numbers/events here are synthetic acceptance fixtures, never measurements.
    def fixture(self, case_id='deny-ws'):
        expected = controller.case_definition(case_id)
        mode = expected['mode']
        opened = mode in ('direct', 'explicit', 'env', 'precedence', 'env-noproxy') or mode.startswith('redirect')
        case = dict(case=case_id, target_port=1234, proxy_a='127.0.0.1:4321', proxy_b='127.0.0.1:4322',
                    group_drained=True, harness_errors=[], runner_rc=0,
                    **{key: expected[key] for key in ('mode', 'tls', 'untrusted', 'redirect_hostname_skip')})
        rows = []
        def event(reason, note='', **values):
            row = dict(seq=len(rows) + 1, time=.01 * (len(rows) + 1), conn=1, wsi=0 if reason == -7 else 1,
                       reason=reason, name='synthetic-' + str(reason), note=note,
                       input_present=False, len=0, hex='', depth=-1, preverify=-1, x509_error=-1)
            row.update(values)
            rows.append(row)
        event(29)
        if mode not in ('deny', 'close', 'silent') and (expected['tls'] or mode == 'redirect-cross'):
            event(58, depth=0, preverify=0 if mode == 'untrusted' else 1,
                  x509_error=19 if mode == 'untrusted' else 0)
        if opened:
            event(24)
            event(3)
        else:
            event(1)
        event(-7, 'proxy-primary-finished', len=1)
        if opened:
            event(8, hex=b'b1-echo'.hex(), len=7)
            event(75)
        event(30)
        event(-7, 'proxy-shutdown-begin')
        event(-7, 'proxy-shutdown-end')
        trace = dict(schema=1, case=case_id, connection=1, events=rows, dropped_events=0,
                     primary_completed=True, shutdown_completed=True, opened=opened, echoed=opened,
                     primary_seconds=21.0 if mode == 'silent' else .1,
                     primary_budget_seconds=expected['primary_budget_seconds'],
                     destroyed=True, destroyed_before_shutdown=True, configured_proxy_absent=True,
                     explicit_proxy=case['proxy_a'] if expected['explicit_proxy'] else '',
                     **{key: expected[key] for key in ('mode', 'tls', 'untrusted', 'redirect_hostname_skip')})
        def proxy(fixture_mode):
            return dict(mode=fixture_mode, lifetime_seconds=40.0 if fixture_mode == 'silent' else 12.0,
                        active=0, events=[], dropped=0)
        a, b = proxy(expected['fixture_mode']), proxy('tunnel')
        selected = b if mode == 'env' else None if mode in ('direct', 'env-noproxy') else a
        if selected is not None:
            flow = [dict(id=1, ev='connect', headers=[], authority='127.0.0.1:1234')]
            if mode == 'deny':
                flow.append(dict(id=1, ev='denied', status=403))
            elif mode == 'close':
                flow.append(dict(id=1, ev='closed_without_response'))
            elif mode == 'silent':
                flow.extend([dict(id=1, ev='silent', lifetime_seconds=40.0), dict(id=1, ev='peer_eof')])
            else:
                flow.extend([dict(id=1, ev='upstream', port=1234), dict(id=1, ev='tunnel')])
            flow.append(dict(id=1, ev='end'))
            selected['events'] = flow
        def journal(status=None):
            events = [] if status is None else [
                dict(ev='request', conn=1, headers=[['Authorization', 'Bearer b1-probe-marker']]),
                dict(ev='response', conn=1, status=status)]
            return dict(events=events, connections=0 if status is None else 1,
                        ended=status is not None, dropped=0)
        source = journal(302 if mode.startswith('redirect') else 403 if mode == 'upgrade403'
                         else 101 if opened else None)
        target = journal(101 if mode.startswith('redirect') else None)
        return case, trace, a, b, source, target

    def reject(self, data, message):
        self.assertIn(message, summarize(*data)['control_errors'])

    def test_runner_failure_cannot_be_hidden_by_empty_harness_errors(self):
        data = self.fixture()
        data[0]['runner_rc'] = 1
        self.assertIn('editor/automation runner failed', summarize(*data)['harness_errors'])

    def test_each_distinct_associated_token_requires_destroy(self):
        data = self.fixture()
        extra = copy.deepcopy(data[1]['events'][0])
        extra.update(wsi=2)
        data[1]['events'].insert(1, extra)
        for seq, event in enumerate(data[1]['events'], 1):
            event['seq'] = seq
        self.reject(data, 'attempt token missing destruction: 2')

    def destruction_only_fixture(self, case_id):
        data = self.fixture(case_id)
        rows = data[1]['events']
        rows[:] = [e for e in rows if e['reason'] != 1]
        destroy = next(e for e in rows if e['reason'] == 30)
        rows.remove(destroy)
        rows.insert(1, destroy)
        for seq, event in enumerate(rows, 1):
            event['seq'] = seq
        return data

    def test_silent_destruction_only_is_reported_without_error(self):
        for case_id in ('silent-ws', 'silent-wss'):
            with self.subTest(case=case_id):
                row = summarize(*self.destruction_only_fixture(case_id))
                self.assertEqual(row['harness_errors'] + row['control_errors'], [])
                self.assertTrue(row['silent_destroy_only'])
                self.assertEqual(row['errors'], [])

    def test_destruction_only_does_not_excuse_other_negative_controls(self):
        for case_id in ('deny-ws', 'close-wss', 'untrusted-wss', 'upgrade403-ws'):
            with self.subTest(case=case_id):
                data = self.destruction_only_fixture(case_id)
                self.reject(data, 'missing terminal callback before matching destruction')
                self.assertFalse(summarize(*data)['silent_destroy_only'])

    def test_silent_shutdown_only_destruction_is_refused(self):
        data = self.destruction_only_fixture('silent-wss')
        rows = data[1]['events']
        destroy = next(e for e in rows if e['reason'] == 30)
        rows.remove(destroy)
        rows.insert(-1, destroy)
        for seq, event in enumerate(rows, 1):
            event['seq'] = seq
        data[1]['destroyed_before_shutdown'] = False
        self.reject(data, 'silent attempt required forced context destruction')
        self.assertFalse(summarize(*data)['silent_destroy_only'])

    def test_repeated_create_and_late_association_keep_raw_identity(self):
        data = self.fixture()
        extra = copy.deepcopy(data[1]['events'][0])
        data[1]['events'][0]['conn'] = 0
        data[1]['events'].insert(1, extra)
        internal = copy.deepcopy(extra)
        internal.update(conn=0, wsi=2)
        data[1]['events'].insert(0, internal)
        for seq, event in enumerate(data[1]['events'], 1):
            event['seq'] = seq
        row = summarize(*data)
        self.assertEqual(row['harness_errors'] + row['control_errors'], [])
        life = lifecycle(data[1])
        self.assertEqual(life['token_lifecycles'][0]['create_sequences'], [2, 3])
        self.assertEqual(life['token_lifecycles'][0]['destroy_sequences'], [6])
        self.assertEqual(len(life['token_lifecycles']), 1)

    def test_malformed_case_trace_and_journal_shapes_are_diagnostics(self):
        mutations = [(index, [], None) for index in range(6)]
        mutations += [(0, 'harness_errors', 'bad'), (0, 'target_port', True),
                      (1, 'events', None), (1, 'events', [None]),
                      (1, 'connection', True), (1, 'primary_seconds', '1'),
                      (1, 'primary_seconds', float('nan')),
                      (1, 'primary_seconds', 10 ** 400),
                      (2, 'events', [None]), (2, 'mode', []),
                      (4, 'events', [None]), (4, 'connections', False)]
        for index, field, value in mutations:
            with self.subTest(index=index, field=field):
                data = list(self.fixture())
                if field == []:
                    data[index] = []
                else:
                    data[index][field] = value
                row = summarize(*data, expected_case_id='deny-ws')
                self.assertTrue(row['harness_errors'] + row['control_errors'])
        for field, value in (('wsi', '1'), ('seq', True), ('hex', None),
                             ('hex', 'xy'), ('input_present', 1), ('name', [])):
            with self.subTest(event_field=field):
                data = self.fixture()
                data[1]['events'][0][field] = value
                row = summarize(*data)
                self.assertTrue(row['harness_errors'] + row['control_errors'])
        data = self.fixture()
        data[2]['events'][0]['headers'] = [[None, 'x']]
        row = summarize(*data)
        self.assertTrue(row['harness_errors'] + row['control_errors'])

    def write_synthetic_matrix(self, run):
        # Disk-format acceptance fixture only. Never used by measured collection.
        manifest = dict(schema=1, commit='synthetic-test', scope='synthetic test only',
                        dirty='', acquisition_only=True, cases=list(EXPECTED),
                        results=[dict(case=c, harness_ok=True) for c in EXPECTED])
        controller.write_json(run / 'manifest.json', manifest)
        (run / 'server.log').write_text('SYNTHETIC\n')
        for case_id in EXPECTED:
            folder = run / case_id
            folder.mkdir()
            for name, value in zip(('case', 'trace', 'proxy-a', 'proxy-b', 'source', 'target'),
                                   self.fixture(case_id)):
                controller.write_json(folder / (name + '.json'), value)
            for name in ('editor.log', 'runner.txt'):
                (folder / name).write_text('SYNTHETIC\n')
        self.hash_synthetic_matrix(run)

    def hash_synthetic_matrix(self, run):
        controller.write_json(run / 'sha256.json', {
            str(p.relative_to(run)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in run.rglob('*') if p.is_file() and p.name != 'sha256.json'})

    def test_malformed_evidence_refuses_and_preserves_passport(self):
        changes = [('manifest.json', []), ('manifest.json', {'cases': [None]}),
                   ('sha256.json', []), ('sha256.json', {'manifest.json': []}),
                   ('deny-ws/case.json', []), ('deny-ws/trace.json', []),
                   ('deny-ws/proxy-a.json', {'events': [None]}),
                   ('deny-ws/source.json', {'events': [None]})]
        for name, bad in changes:
            with self.subTest(name=name, bad=bad), tempfile.TemporaryDirectory() as temp:
                run = pathlib.Path(temp) / 'run'
                run.mkdir()
                out = pathlib.Path(temp) / 'passport.md'
                out.write_bytes(b'existing real passport sentinel\n')
                self.write_synthetic_matrix(run)
                controller.write_json(run / name, bad)
                if name != 'sha256.json':
                    self.hash_synthetic_matrix(run)
                with contextlib.redirect_stderr(io.StringIO()) as errors:
                    self.assertEqual(generate(run, out), 1)
                self.assertTrue(errors.getvalue())
                self.assertEqual(out.read_bytes(), b'existing real passport sentinel\n')

    def test_synthetic_complete_disk_matrix_and_integrity_refusal(self):
        with tempfile.TemporaryDirectory() as temp:
            run = pathlib.Path(temp) / 'run'
            run.mkdir()
            out = pathlib.Path(temp) / 'passport.md'
            self.write_synthetic_matrix(run)
            self.assertEqual(generate(run, out), 0)
            before = out.read_bytes()
            self.assertIn(b'synthetic test only', before)
            for case_id in EXPECTED:
                self.assertIn(('### ' + case_id).encode(), before)
            path = run / 'deny-ws' / 'trace.json'
            trace = json.loads(path.read_text())
            trace['dropped_events'] = 1
            controller.write_json(path, trace)
            with contextlib.redirect_stderr(io.StringIO()) as errors:
                self.assertEqual(generate(run, out), 1)
            self.assertIn('artifact hash mismatch', errors.getvalue())
            self.assertIn('dropped trace events', errors.getvalue())
            self.assertEqual(out.read_bytes(), before)

    def test_all_named_reference_cases_pass(self):
        for case_id in EXPECTED:
            with self.subTest(case=case_id):
                row = summarize(*self.fixture(case_id))
                self.assertEqual(row['harness_errors'], [])
                self.assertEqual(row['control_errors'], [])

    def test_empty_error_is_measurement_not_403_text(self):
        row = summarize(*self.fixture())
        self.assertEqual(row['errors'][0]['input_present'], False)
        self.assertEqual(row['errors'][0]['length'], 0)
        self.assertNotIn('403', json.dumps(row['errors']))
        manifest = dict(commit='synthetic-test', scope='synthetic test only', dirty='')
        text = render(manifest, [row], pathlib.Path('/synthetic'))
        self.assertIn('not production policy', text)
        self.assertIn('Remaining release gates', text)

    def test_missing_or_duplicate_rows_fail(self):
        self.assertEqual(check_cases(list(EXPECTED)), [])
        self.assertTrue(check_cases(list(EXPECTED[:-1])))
        self.assertTrue(check_cases(list(EXPECTED) + [EXPECTED[0]]))

    def test_denial_send_failure_is_not_denial(self):
        data = self.fixture()
        data[2]['events'][1] = dict(id=1, ev='io_error', errno=32)
        self.reject(data, 'missing successful fixture witness: denied')
        self.reject(data, 'proxy stimulus substituted by IO/deadline/stop/upstream')
        data = self.fixture()
        data[2]['events'][1]['status'] = 401
        self.reject(data, 'missing successful fixture witness: denied')

    def test_close_and_silent_need_their_own_witnesses(self):
        for case_id, witness in (('close-ws', 'closed_without_response'), ('silent-wss', 'silent')):
            with self.subTest(case=case_id):
                data = self.fixture(case_id)
                data[2]['events'] = [e for e in data[2]['events'] if e['ev'] != witness]
                data[2]['events'].insert(1, dict(id=1, ev='io_error', errno=104))
                self.reject(data, 'missing successful fixture witness: ' + witness)
                self.reject(data, 'proxy stimulus substituted by IO/deadline/stop/upstream')

    def test_tunnel_eof_without_failed_certificate_is_not_untrusted(self):
        data = self.fixture('untrusted-wss')
        for event in data[1]['events']:
            if event['reason'] == 58:
                event['reason'] = -7
                event['note'] = 'removed-cert'
        data[2]['events'].insert(-1, dict(id=1, ev='io_error', errno=104))
        self.reject(data, 'untrusted wss lacks associated failed certificate before terminal/destruction')

    def test_certificate_witness_must_belong_to_attempt_and_token(self):
        for field, value in (('conn', 2), ('wsi', 2), ('preverify', 1), ('x509_error', 0), ('depth', -1)):
            with self.subTest(field=field):
                data = self.fixture('untrusted-wss')
                next(e for e in data[1]['events'] if e['reason'] == 58)[field] = value
                self.reject(data, 'untrusted wss lacks associated failed certificate before terminal/destruction')
        data = self.fixture('untrusted-wss')
        cert = next(e for e in data[1]['events'] if e['reason'] == 58)
        terminal = next(e for e in data[1]['events'] if e['reason'] == 1)
        cert['seq'], terminal['seq'] = terminal['seq'], cert['seq']
        self.reject(data, 'untrusted wss lacks associated failed certificate before terminal/destruction')

    def test_manifest_directory_and_proxy_option_binding(self):
        data = self.fixture('explicit-wss')
        row = summarize(*data, expected_case_id='env-wss')
        self.assertIn('immutable case directory identity', row['control_errors'])
        data[1]['explicit_proxy'] = ''
        self.reject(data, 'immutable explicit proxy option')
        data = self.fixture('env-wss')
        data[1]['configured_proxy_absent'] = False
        self.reject(data, 'engine configured proxy not absent')

    def test_wss_cannot_be_relabelled_ws_or_skip_hostname(self):
        for field, value in (('tls', False), ('redirect_hostname_skip', True), ('mode', 'direct'), ('untrusted', True)):
            with self.subTest(field=field):
                data = self.fixture('explicit-wss')
                data[0][field] = value
                data[1][field] = value
                self.reject(data, 'immutable case binding: ' + field)
        data = self.fixture('explicit-wss')
        for event in data[1]['events']:
            if event['reason'] == 58:
                event['conn'] = 2
        self.reject(data, 'trusted wss lacks associated successful certificate evidence')

    def test_silent_short_budget_or_lifetime_or_fixture_end_is_refused(self):
        data = self.fixture('silent-ws')
        data[1]['primary_budget_seconds'] = 8.0
        self.reject(data, 'immutable primary observation budget')
        data = self.fixture('silent-wss')
        data[2]['lifetime_seconds'] = 12.0
        self.reject(data, 'immutable fixture lifetime')
        for replacement in ('hold_deadline', 'stopped', 'io_error'):
            with self.subTest(replacement=replacement):
                data = self.fixture('silent-ws')
                next(e for e in data[2]['events'] if e['ev'] == 'peer_eof')['ev'] = replacement
                self.reject(data, 'proxy stimulus substituted by IO/deadline/stop/upstream')
                self.reject(data, 'silent fixture lacks client EOF before teardown')

    def test_missing_or_contradictory_terminal_lifecycle_is_refused(self):
        data = self.fixture()
        next(e for e in data[1]['events'] if e['reason'] == 1)['reason'] = -7
        self.reject(data, 'missing terminal callback before matching destruction')
        data = self.fixture()
        next(e for e in data[1]['events'] if e['reason'] == 30)['reason'] = -7
        self.reject(data, 'contradictory lifecycle summary: destroyed')
        for field in ('primary_completed', 'destroyed', 'destroyed_before_shutdown', 'shutdown_completed'):
            with self.subTest(field=field):
                data = self.fixture()
                data[1][field] = False
                self.reject(data, 'contradictory lifecycle summary: ' + field)
        for field in ('opened', 'echoed'):
            with self.subTest(field=field):
                data = self.fixture('explicit-wss')
                data[1][field] = False
                self.reject(data, 'contradictory lifecycle summary: ' + field)
        data = self.fixture('silent-ws')
        destroy = next(e for e in data[1]['events'] if e['reason'] == 30)
        shutdown = next(e for e in data[1]['events'] if e.get('note') == 'proxy-shutdown-begin')
        destroy['seq'], shutdown['seq'] = shutdown['seq'], destroy['seq']
        data[1]['destroyed_before_shutdown'] = False
        self.reject(data, 'silent attempt required forced context destruction')

    def test_header_leak_and_false_negative_are_rejected(self):
        data = self.fixture()
        data[2]['events'][0]['headers'] = [['authorization', 'Bearer b1-probe-marker']]
        self.reject(data, 'target Authorization present in CONNECT')
        data[1]['opened'] = True
        self.reject(data, 'contradictory lifecycle summary: opened')

    def test_positive_control_and_environment_route_remain_distinct(self):
        data = self.fixture('explicit-ws')
        for event in data[1]['events']:
            if event['reason'] in (3, 8):
                event['reason'] = -7
        data[1].update(opened=False, echoed=False)
        self.reject(data, 'positive open/echo/Upgrade control failed')
        data = self.fixture('env-wss')
        before = summarize(*data)
        data[3]['events'].clear()
        after = summarize(*data)
        self.assertEqual(before['route'], 'B')
        self.assertEqual(after['route'], 'direct')
        self.assertEqual(after['harness_errors'], [])
        self.assertEqual(after['control_errors'], [])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--run', type=pathlib.Path)
    parser.add_argument('--out', type=pathlib.Path)
    parser.add_argument('--check-case', type=pathlib.Path)
    parser.add_argument('--selftest', action='store_true')
    args = parser.parse_args()
    if args.selftest:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(PassportTests)
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    if args.check_case is None and (args.run is None or args.out is None):
        parser.error('--run and --out, or --check-case, are required')
    try:
        if args.check_case is not None:
            folder = args.check_case.resolve()
            values = [json.loads((folder / (name + '.json')).read_text())
                      for name in ('case', 'trace', 'proxy-a', 'proxy-b', 'source', 'target')]
            row = summarize(*values, expected_case_id=folder.name)
            errors = row['harness_errors'] + row['control_errors']
            print(json.dumps({'case': folder.name, 'errors': errors, 'complete_matrix': False}, indent=2))
            return 1 if errors else 0
        return generate(args.run.resolve(), args.out)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print('invalid evidence: ' + str(error), file=sys.stderr)
        return 1

if __name__ == '__main__':
    sys.exit(main())
