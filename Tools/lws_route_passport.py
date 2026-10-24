#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
import argparse
import base64
import errno
import copy
import hashlib
import json
import math
import os
import pathlib
import tempfile
import unittest

MODES = ('precreated', 'fallback', 'held', 'held-reuse', 'destroy-active', 'untrusted', 'redirect')
CASES = tuple((mode + '-' + scheme + '-' + provider, mode, scheme == 'wss', provider == 'provided', 'v4')
              for mode in MODES for scheme in ('ws', 'wss') for provider in ('generated', 'provided')
              if (scheme == 'wss' or (mode in ('precreated', 'fallback', 'destroy-active', 'redirect') and provider == 'generated')))
CASES += tuple(('ipv6-' + form + '-' + scheme, 'precreated', scheme == 'wss', False, form)
               for form in ('bracket-inline', 'bare-port', 'bracket-port') for scheme in ('ws', 'wss'))

def definition(name):
    for ident, mode, tls, provided, form in CASES:
        if ident == name:
            return dict(case=ident, mode=mode, tls=tls, provided=provided, form=form)
    raise ValueError('unknown immutable case')

def read_object(path):
    value = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(value, dict):
        raise ValueError('root must be object: ' + path.name)
    return value

def typed_events(trace):
    if not isinstance(trace, dict):
        raise ValueError('trace root')
    rows = trace.get('events')
    if not isinstance(rows, list) or not rows:
        raise ValueError('trace events')
    for row in rows:
        if not isinstance(row, dict):
            raise ValueError('event object')
        for name in ('seq', 'conn', 'wsi', 'reason', 'len', 'depth', 'x509_error', 'preverify', 'flags'):
            if type(row.get(name)) is not int:
                raise ValueError('event integer: ' + name)
        if type(row.get('input_present')) is not bool or not isinstance(row.get('note'), str) or not isinstance(row.get('name'), str):
            raise ValueError('event input/note')
        if not isinstance(row.get('hex'), str) or len(row['hex']) % 2:
            raise ValueError('event hex')
        bytes.fromhex(row['hex'])
        if type(row.get('time')) not in (int, float) or not math.isfinite(row['time']):
            raise ValueError('event time')
    if [e['seq'] for e in rows] != list(range(1, len(rows) + 1)):
        raise ValueError('trace sequence')
    return rows

def notes(rows, phase):
    found = []
    for event in rows:
        if not event['note'].startswith('{'):
            continue
        row = json.loads(event['note'])
        if not isinstance(row, dict):
            raise ValueError('phase note object')
        if row.get('phase') == phase:
            found.append((event['seq'], row))
    return found

def lifecycle_index(rows):
    # A conn0 CREATE may precede association; repeated CREATE does not mean a new token.
    associated = {e['wsi'] for e in rows if e['conn'] > 0 and e['wsi'] > 0}
    intervals, problems = {}, []
    for token in associated:
        events = [e for e in rows if e['wsi'] == token]
        owners = {e['conn'] for e in events if e['conn'] > 0}
        creates = [e for e in events if e['name'] == 'WSI_CREATE']
        destroys = [e for e in events if e['name'] == 'WSI_DESTROY']
        if len(owners) != 1 or not creates or len(destroys) != 1:
            problems.append('token ownership/create/destroy'); continue
        born, dead = min(e['seq'] for e in creates), destroys[0]['seq']
        owner = next(iter(owners))
        linked = min(e['seq'] for e in events if e['conn'] == owner)
        if born >= dead or linked < born or any(e['seq'] >= dead for e in creates):
            problems.append('destroy precedes create/association'); continue
        if any(e['name'] != 'WSI_DESTROY' and not born <= e['seq'] < dead for e in events):
            problems.append('callback outside token lifetime'); continue
        intervals[token] = dict(conn=owner, born=born, linked=linked, dead=dead)
    return intervals, problems

def lifecycle(rows):
    return lifecycle_index(rows)[1]

def live_witness(value, seq, intervals):
    token, conn = value.get('wsi_token'), value.get('conn')
    if type(token) is not int or type(conn) is not int:
        return ['identity lifecycle field type']
    life = intervals.get(token)
    if life is None or life['conn'] != conn:
        return ['identity token not associated with connection']
    if not life['born'] < seq < life['dead']:
        return ['identity outside associated live interval']
    return []

CAPABILITY_ERRNOS = {errno.EAFNOSUPPORT, errno.EPROTONOSUPPORT, errno.EADDRNOTAVAIL}

def immutable(result):
    if not isinstance(result, dict):
        raise ValueError('result object')
    expected = definition(result.get('case'))
    for key in ('mode', 'tls', 'provided', 'form'):
        if type(result.get(key)) is not type(expected[key]) or result[key] != expected[key]:
            raise ValueError('immutable ' + key)
    return expected

def unsupported(result, evidence):
    expected = immutable(result)
    if result.get('abort_matrix', False) is not False or result.get('errors', []) != []:
        raise ValueError('unsupported acquisition aborted or failed')
    if result.get('unsupported_ipv6') is not True or expected['form'] == 'v4':
        raise ValueError('unsupported requires an IPv6 case')
    if (not isinstance(evidence, dict) or evidence.get('operation') != 'bind-ipv6-loopback'
            or evidence.get('family') != 'AF_INET6' or evidence.get('host') != '::1'
            or type(evidence.get('errno')) is not int or evidence['errno'] not in CAPABILITY_ERRNOS
            or result.get('errno') != evidence['errno'] or result.get('group_drained') is not True):
        raise ValueError('unsupported capability evidence')

def index_state(obs):
    if not isinstance(obs, dict) or type(obs.get('known')) is not bool:
        raise ValueError('index observation type')
    if obs['known']:
        if type(obs.get('value')) is not int:
            raise ValueError('known index value type')
        return obs['value']
    if 'value' in obs:
        raise ValueError('unavailable index invented value')
    return None

def identity(value, tls, supplied, route=None):
    problems = []
    for key in ('conn', 'route', 'actual_route', 'wsi_token', 'ssl_token'):
        if type(value.get(key)) is not int or value[key] < 0:
            problems.append('identity integer ' + key)
    for key in ('ssl_exists', 'wsi_user', 'ctx_exists', 'ctx_supplied', 'index_known', 'index_matches_wsi'):
        if type(value.get(key)) is not bool:
            problems.append('identity bool ' + key)
    if value.get('ctx_origin') not in ('none', 'supplied', 'other'):
        problems.append('identity ctx_origin')
    try:
        index_state(value.get('index'))
    except ValueError as error:
        problems.append(str(error))
    if problems:
        return problems
    if value['conn'] <= 0 or value['wsi_token'] <= 0:
        problems.append('identity lacks owned live token')
    if not value['wsi_user'] or (route is not None and (value['actual_route'] != route or value['route'] != route)):
        problems.append('observed: actual route/user mismatch')
    if tls and (not value['ssl_exists'] or value['ssl_token'] <= 0 or not value['ctx_exists']):
        problems.append('observed: missing live SSL/context')
    if value['index_known'] != value['index']['known']:
        problems.append('identity index availability inconsistent')
    if value['ctx_supplied'] != (value['ctx_origin'] == 'supplied'):
        problems.append('identity origin inconsistent')
    if supplied and tls and not value['ctx_supplied']:
        problems.append('observed: supplied context not used')
    return problems

def provenance(rows, trace):
    """One evidence consumer for validation AND every ownership summary, including non-open TLS."""
    intervals, problems = lifecycle_index(rows)
    valid, matched_certs, matched_opens, ssl_owners = [], {}, {}, {}
    phases = ('ssl-before-route', 'ssl-after-route', 'ssl-open', 'neighbor-ssl-open', 'verify-identity')
    records = [(seq, value, phase) for phase in phases for seq, value in notes(rows, phase)]
    for phase in ('create-before', 'create-after', 'tls-init-before', 'tls-init-after'):
        for seq, value in notes(rows, phase):
            watch = value.get('watch')
            if not isinstance(watch, dict):
                problems.append('operation live watch absent'); continue
            # An explicit no-watch record makes no ownership or live-SSL claim.
            if (type(watch.get('conn')) is int and watch['conn'] == 0
                    and type(watch.get('wsi_token')) is int and watch['wsi_token'] == 0
                    and type(watch.get('ssl_token')) is int and watch['ssl_token'] == 0
                    and watch.get('ssl_exists') is False):
                continue
            records.append((seq, watch, 'watch'))
    for seq, value, phase in sorted(records, key=lambda row: row[0]):
        errors = identity(value, trace['tls'], trace['provided'])
        errors.extend(live_witness(value, seq, intervals))
        route = value.get('route')
        if type(route) is not int or route not in (trace['first_route'], trace['next_route']):
            errors.append('identity undeclared route')
        conn = value.get('conn')
        expected_route = (trace['selected_route'] if conn == trace['final_connection'] else
                          trace['next_route'] if conn == trace['neighbor_connection'] and conn > 0 else
                          trace['first_route'] if conn == trace['first_connection'] else None)
        if expected_route is None or route != expected_route:
            errors.append('identity connection route mismatch')
        if value.get('actual_route') != route:
            errors.append('observed: actual route/user mismatch')
        if (value.get('ctx_exists') != value.get('ssl_exists')
                or value.get('ctx_exists') != (value.get('ctx_origin') in ('supplied', 'other'))):
            errors.append('identity context existence inconsistent')
        if value.get('ssl_exists') is False and value.get('ssl_token') != 0:
            errors.append('identity absent SSL token')
        if value.get('index_matches_wsi') is True and (value.get('index_known') is not True or value.get('ssl_exists') is not True):
            errors.append('identity index match without observation')
        event = rows[seq-1]
        if phase != 'watch' and event['conn'] != value.get('conn'):
            errors.append('identity marker connection mismatch')
        if phase in ('ssl-open', 'neighbor-ssl-open'):
            opens = [e for e in rows if e['conn'] == value.get('conn') and e['name'] == 'ESTABLISHED']
            if len(opens) != 1 or opens[0]['seq'] >= seq or opens[0]['wsi'] != value.get('wsi_token'):
                errors.append('identity actual open association')
            else:
                matched_opens.setdefault(opens[0]['seq'], []).append(seq)
        if phase == 'verify-identity':
            # The native hook records this note after Handle appended the actual certificate callback.
            # Match the latest preceding WSI callback of this connection, then enforce one note per callback.
            previous = [e for e in rows if e['seq'] < seq and e['conn'] == value.get('conn') and e['wsi'] > 0]
            cert = previous[-1] if previous else None
            if (cert is None or cert['name'] != 'PERFORM_SERVER_CERT_VERIFICATION'
                    or cert['wsi'] != value.get('wsi_token') or type(value.get('preverify')) is not int
                    or value['preverify'] != cert['preverify']):
                errors.append('identity actual verification association')
            else:
                matched_certs.setdefault(cert['seq'], []).append(seq)
            if any(value.get(k) is not True for k in ('known_wsi', 'ssl_input', 'store_ssl', 'wsi_user')):
                errors.append('observed: verify association mismatch')
        if value.get('ssl_exists') is True and type(value.get('ssl_token')) is int:
            token = value['ssl_token']
            owner = (value.get('conn'), value.get('wsi_token'), route, value.get('ctx_origin'))
            if token in ssl_owners and ssl_owners[token] != owner:
                errors.append('SSL token ownership/origin changed')
            ssl_owners[token] = owner
        problems.extend(errors)
        if not errors and value.get('ssl_exists') is True:
            valid.append(value)
    for cert in (e for e in rows if e['name'] == 'PERFORM_SERVER_CERT_VERIFICATION'):
        if len(matched_certs.get(cert['seq'], [])) != 1:
            problems.append('certificate callback requires one associated provenance record')
    for opened in (e for e in rows if e['name'] == 'ESTABLISHED'):
        if len(matched_opens.get(opened['seq'], [])) != 1:
            problems.append('open callback requires one associated provenance record')
    # Any malformed/unassociated record invalidates the aggregate ownership claim, even if another note is valid.
    used = not problems and bool(valid) and all(v['ctx_supplied'] and v['ctx_origin'] == 'supplied' for v in valid)
    return dict(errors=problems, supplied_used=used, live_records=len(valid))

def operations(rows):
    result, problems = [], []
    routes = sorted({v.get('route') for _, v in notes(rows, 'create-before') if type(v.get('route')) is int})
    for route in routes:
        created = None
        for op in ('create', 'tls-init'):
            before = [(n, v) for n, v in notes(rows, op + '-before') if v.get('route') == route]
            after = [(n, v) for n, v in notes(rows, op + '-after') if v.get('route') == route]
            skipped = [(n, v) for n, v in notes(rows, 'tls-init-skipped') if v.get('route') == route]
            if op == 'tls-init' and created is False:
                if before or after or len(skipped) != 1:
                    problems.append('failed create requires explicit skipped init')
                result.append(dict(route=route, operation=op, status='skipped', motion='unobserved'))
                continue
            if len(before) != 1 or len(after) != 1 or before[0][0] >= after[0][0]:
                problems.append('incomplete operation %d/%s' % (route, op))
                result.append(dict(route=route, operation=op, status='partial', motion='unobserved'))
                continue
            left, right = before[0][1], after[0][1]
            if type(left.get('value')) is not bool or type(right.get('value')) is not bool:
                problems.append('operation return type')
            try:
                start, end = index_state(left.get('index')), index_state(right.get('index'))
                motion = 'unavailable' if start is None or end is None else 'changed' if start != end else 'unchanged'
            except ValueError as error:
                problems.append(str(error)); motion = 'malformed'
            if op == 'create':
                created = right.get('value')
            result.append(dict(route=route, operation=op, status='returned-success' if right.get('value') is True else 'returned-failure',
                               motion=motion, before=before[0][0], after=after[0][0]))
    if not routes:
        problems.append('no route operation records')
    return result, problems

def ownership(rows, trace, evidence):
    stages = notes(rows, 'ownership')
    problems = []
    if [v.get('stage') for _, v in stages] != list(range(9)):
        problems.append('ownership stage chain')
    labels=['cleanup-start','vhost-before' if trace['mode']=='destroy-active' else 'vhost-skipped-before',
            'vhost-after' if trace['mode']=='destroy-active' else 'vhost-skipped-after','retirement-before',
            'retirement-after','shutdown-before','shutdown-after','caller-before','caller-after']
    if [v.get('label') for _,v in stages] != labels:
        problems.append('ownership stage labels')
    if len(stages)==9:
        begins=[e['seq'] for e in rows if e['note']=='shutdown-begin']
        ends=[e['seq'] for e in rows if e['note']=='shutdown-end']
        if len(begins)!=1 or len(ends)!=1 or not stages[5][0] < begins[0] < ends[0] < stages[6][0]:
            problems.append('joined destruction stage window')
        if trace['mode']=='destroy-active':
            before,after=notes(rows,'destroy-before'),notes(rows,'destroy-after')
            if len(before)!=1 or len(after)!=1 or not before[0][0] < stages[1][0] < stages[2][0] < after[0][0] < stages[3][0]:
                problems.append('vhost destruction stage window')
    counts = [v.get('frees') for _, v in stages]
    if any(type(n) is not int or n < 0 for n in counts) or counts != sorted(counts):
        problems.append('ownership count monotonicity')
    destroyed = trace.get('final_ctx_destructions')
    if not isinstance(destroyed, list) or any(not isinstance(d, dict) or type(d.get('stage')) is not int
            or not 0 <= d['stage'] <= 8 or type(d.get('count')) is not int for d in destroyed):
        return problems + ['final context destruction records'], 'malformed'
    if [d['count'] for d in destroyed] != list(range(1, len(destroyed) + 1)):
        problems.append('final context destruction count chain')
    if [d['stage'] for d in destroyed] != sorted(d['stage'] for d in destroyed):
        problems.append('final context destruction stage order')
    if len(stages) == 9 and not problems:
        if counts[6] != trace['frees_after_context'] or counts[8] != trace['frees_after_caller'] or counts[8] != len(destroyed):
            problems.append('ownership final totals')
        for (_, value) in stages:
            lower = sum(d['stage'] < value['stage'] for d in destroyed)
            upper = sum(d['stage'] <= value['stage'] for d in destroyed)
            if not lower <= value['frees'] <= upper:
                problems.append('ownership snapshot/destruction mismatch')
    used = evidence['supplied_used'] and not problems
    if not trace['provided']:
        label = 'generated context; no supplied ownership claim'
    elif not used:
        label = 'supplied use unestablished'
    elif not destroyed:
        label = 'missing final destruction'
    elif len(destroyed) > 1:
        label = 'multiple final destructions'
    elif destroyed[0]['stage'] < 5:
        label = 'early final destruction at stage %d' % destroyed[0]['stage']
    elif destroyed[0]['stage'] == 5:
        label = 'final destruction during context shutdown'
    elif destroyed[0]['stage'] == 7 and trace['frees_after_context'] == 0:
        label = 'exercised supplied context finally destroyed by caller'
    else:
        label = 'late/unexpected final destruction stage %d' % destroyed[0]['stage']
    return problems, label

def header_values(journal):
    requests = [e for e in journal['events'] if e.get('ev') == 'request']
    headers = []
    for request in requests:
        pairs = request.get('headers')
        if not isinstance(pairs, list) or any(not isinstance(p, list) or len(p) != 2
                or not all(isinstance(v, str) for v in p) for p in pairs):
            raise ValueError('target header shape')
        headers.append({k.lower(): v for k, v in pairs})
    return headers

def peer_proof(result, relay, journal, peer, tls):
    problems = []
    connects = [e for e in relay['events'] if e['ev'] == 'connect']
    hellos = [e for e in relay['events'] if e['ev'] == 'client_hello']
    if result['mode'] == 'redirect':
        connects, hellos = connects[:1], hellos[:1]
    if not connects:
        problems.append('required proxy stimulus absent')
    for event in connects:
        if (event.get('authority') != 'localhost:%d' % result['target_port']
                or event.get('peer_host') != peer['host'] or event.get('peer_port') != peer['port']):
            problems.append('observed: actual proxy peer/CONNECT mismatch')
    if tls and (not hellos or any(e.get('sni') != 'localhost' for e in hellos)):
        problems.append('required named TLS ClientHello absent')
    if journal is not None:
        try:
            headers = header_values(journal)
            if not headers:
                problems.append('target Upgrade witness absent')
            for header in headers:
                if header.get('host') != 'localhost:%d' % result['target_port'] or header.get('authorization') != 'Bearer route-probe-marker':
                    problems.append('observed: target Host/credential mismatch')
        except ValueError as error:
            problems.append(str(error))
    return problems

def connection_proof(rows, conn, route, phase, tls, supplied):
    problems = []
    opened = [e for e in rows if e['conn'] == conn and e['name'] == 'ESTABLISHED']
    witnesses = [(seq, v) for seq, v in notes(rows, phase) if v.get('conn') == conn]
    if len(opened) != 1 or len(witnesses) != 1:
        return ['independent open/SSL witness absent']
    seq, witness = witnesses[0]
    problems.extend(identity(witness, tls, supplied, route))
    if seq <= opened[0]['seq']:
        problems.append('SSL open witness precedes open')
    if tls:
        certs = [e for e in rows if e['conn'] == conn and e['name'] == 'PERFORM_SERVER_CERT_VERIFICATION']
        if not any(e['depth'] == 0 and e['preverify'] == 1 and e['x509_error'] == 0 for e in certs):
            problems.append('verified leaf absent')
        checks = [v for _, v in notes(rows, 'verify-identity') if v.get('conn') == conn]
        if not checks:
            problems.append('verify identity absent')
        for check in checks:
            problems.extend(identity(check, True, supplied, route))
            if any(check.get(k) is not True for k in ('known_wsi', 'ssl_input', 'store_ssl', 'wsi_user')):
                problems.append('observed: verify association mismatch')
            if any(check.get(k) != witness.get(k) for k in ('wsi_token', 'ssl_token', 'actual_route')):
                problems.append('verify/open identity differs')
    return problems

def retirement_stages(rows, conn):
    tokens = {e['wsi'] for e in rows if e['conn'] == conn and e['wsi'] > 0}
    before = notes(rows, 'destroy-before'); after = notes(rows, 'destroy-after')
    shutdown = [e['seq'] for e in rows if e['note'] == 'shutdown-begin']
    result = []
    for token in sorted(tokens):
        endings = [e['seq'] for e in rows if e['wsi'] == token and e['name'] == 'WSI_DESTROY']
        end = endings[0] if len(endings) == 1 else None
        label = 'missing/duplicate'
        if end is not None and shutdown:
            if before and end < before[0][0]: label = 'before destroy request'
            elif after and end <= after[0][0]: label = 'during vhost destroy'
            elif end < shutdown[0]: label = 'during retirement wait'
            else: label = 'only during context shutdown'
        result.append(dict(token=token, destroy_seq=end, stage=label))
    return result

def validate_case(result, trace, relay_a, relay_b, source, target, neighbor):
    try:
        expected = immutable(result)
    except (ValueError, TypeError) as error:
        return [str(error)]
    problems = []
    if result.get('group_drained') is not True: problems.append('group not drained')
    ordering = result.get('journal_order')
    if (not isinstance(ordering, dict) or ordering.get('child_group_drained') is not True
            or ordering.get('relays_naturally_idle') != [True, True]
            or any(type(v) is not bool for v in ordering.get('relays_naturally_idle', []))
            or ordering.get('before_fixture_stop') is not True):
        problems.append('authoritative relay ordering unproven')
    if type(result.get('runner_rc')) is not int: problems.append('runner rc missing')
    if type(result.get('target_port')) is not int or not 1 <= result['target_port'] <= 65535: problems.append('target port')
    if not isinstance(result.get('gate'), dict): problems.append('gate shape')
    if not isinstance(result.get('errors'), list) or result['errors']: problems.append('parent acquisition errors')
    for key in ('peer_a', 'peer_b'):
        peer = result.get(key)
        if not isinstance(peer, dict) or peer.get('host') not in ('127.0.0.1', '::1') or type(peer.get('port')) is not int or not 1 <= peer['port'] <= 65535:
            problems.append('peer endpoint type')
    for relay in (relay_a, relay_b):
        if not isinstance(relay, dict) or not isinstance(relay.get('events'), list):
            problems.append('relay shape'); continue
        if type(relay.get('dropped')) is not int or relay['dropped'] != 0 or relay.get('active') != 0:
            problems.append('relay loss/live')
        prior_hellos = {}
        for event in relay['events']:
            if not isinstance(event, dict) or not isinstance(event.get('ev'), str):
                problems.append('relay event shape'); continue
            if event['ev'] in ('fixture_error', 'hold_deadline', 'hold_stopped', 'stopped', 'relay_deadline'):
                problems.append('fixture stimulus failed')
            if event['ev'] == 'client_hello':
                ident, stamp, size = event.get('id'), event.get('t'), event.get('record_bytes')
                if (type(ident) is int and ident > 0 and type(stamp) in (int, float)
                        and math.isfinite(stamp) and type(size) is int and 9 <= size <= 18437
                        and isinstance(event.get('sni'), str)):
                    prior_hellos[ident] = stamp
            if event['ev'] == 'peer_reset':
                ident, stamp = event.get('id'), event.get('t')
                if (type(ident) is not int or ident <= 0 or ident not in prior_hellos
                        or type(stamp) not in (int, float) or not math.isfinite(stamp)
                        or stamp <= prior_hellos[ident] or event.get('operation') != 'recv-client'
                        or type(event.get('errno')) is not int or event['errno'] != errno.ECONNRESET
                        or event.get('phase') != 'after-forwarded-client-hello'):
                    problems.append('peer reset provenance')
            if event['ev'] == 'connect':
                headers = event.get('headers')
                if not isinstance(headers, list) or any(not isinstance(p, list) or len(p) != 2
                        or not all(isinstance(v, str) for v in p) for p in headers):
                    problems.append('CONNECT header shape')
                elif any(p[0].lower() == 'authorization' for p in headers): problems.append('credential in CONNECT')
    for journal in (source, target, neighbor):
        if not isinstance(journal, dict) or not isinstance(journal.get('events'), list):
            problems.append('target journal shape')
        elif type(journal.get('dropped')) is not int or journal['dropped'] != 0 or type(journal.get('connections')) is not int or any(not isinstance(e, dict) for e in journal['events']):
            problems.append('target journal loss/type')
    if problems: return problems
    if trace is None: return ['no final trace; partial/crashed acquisition']
    try: rows = typed_events(trace)
    except (ValueError, TypeError, OverflowError) as error: return [str(error)]
    for key in ('case', 'mode', 'tls', 'provided'):
        if type(trace.get(key)) is not type(expected[key]) or trace[key] != expected[key]: problems.append('trace ' + key)
    for key in ('harness_ok', 'first_primary', 'final_primary', 'opened', 'echoed', 'retired_before_fallback', 'retired_before_shutdown', 'shutdown_completed'):
        if type(trace.get(key)) is not bool: problems.append('trace bool ' + key)
    for key in ('frees_after_context', 'frees_after_caller', 'first_connection', 'final_connection', 'neighbor_connection', 'first_route', 'next_route', 'selected_route'):
        if type(trace.get(key)) is not int: problems.append('trace integer ' + key)
    if trace.get('schema') != 1 or type(trace.get('dropped')) is not int or trace['dropped'] != 0 or trace.get('shutdown_completed') is not True:
        problems.append('trace schema/loss/shutdown')
    if trace.get('harness_ok') is not True or result['runner_rc'] != 0: problems.append('acquisition failed')
    if problems: return problems
    evidence = provenance(rows, trace); problems.extend(evidence['errors'])
    ops, op_errors = operations(rows); problems.extend(op_errors)
    if {op['route'] for op in ops} != {trace['first_route'], trace['next_route']}:
        problems.append('declared route operations missing')
    for phase in ('create-before','create-after','tls-init-before','tls-init-after','tls-init-skipped'):
        for _, value in notes(rows, phase):
            if type(value.get('route')) is not int or value['route'] not in (trace['first_route'],trace['next_route']):
                problems.append('operation route identity')
    owner_errors, owner_label = ownership(rows, trace, evidence); problems.extend(owner_errors)
    if trace['final_primary'] is not True: problems.append('primary observation incomplete')
    expected_selected = trace['next_route'] if expected['mode']=='precreated' or (expected['mode']=='fallback' and trace['retired_before_fallback']) else trace['first_route']
    if trace['selected_route'] != expected_selected:
        problems.append('observed: explicit nondefault route selection mismatch')
    route_refused = any(op['route'] == trace['selected_route'] and op['status'] == 'returned-failure' for op in ops)
    if not route_refused or trace['opened']:
        problems.extend(peer_proof(result, relay_a, source if trace['opened'] or expected['mode'] == 'redirect' else None, result['peer_a'], expected['tls']))
    if trace['opened']:
        if not trace['echoed']: problems.append('positive open lacked echo')
        problems.extend(connection_proof(rows, trace['final_connection'], trace['selected_route'], 'ssl-open', expected['tls'], expected['provided']))
    if expected['mode'] == 'untrusted' and (not route_refused or trace['opened']):
        bad = [e for e in rows if e['name'] == 'PERFORM_SERVER_CERT_VERIFICATION' and e['preverify'] == 0 and e['x509_error'] != 0]
        if not bad: problems.append('untrusted failed-certificate stimulus absent')
        if trace['opened']: problems.append('observed: untrusted peer opened')
    if expected['mode'] == 'redirect' and not route_refused:
        if not any(e.get('ev') == 'response' and e.get('status') == 302 for e in source['events']): problems.append('redirect response stimulus absent')
    if expected['mode'] == 'fallback' and trace['opened']:
        markers = [e for e in rows if e['note'] == 'fallback-retired']
        second = [e for e in rows if e['conn'] == trace['final_connection'] and e['name'] == 'connect>']
        tokens = {e['wsi'] for e in rows if e['conn'] == trace['first_connection'] and e['wsi'] > 0}
        if not markers or markers[0]['len'] != 1 or not second or markers[0]['seq'] >= second[0]['seq'] or not trace['retired_before_fallback']:
            problems.append('fallback before retirement')
        elif any(e['seq'] >= second[0]['seq'] for e in rows if e['wsi'] in tokens and e['name'] == 'WSI_DESTROY'):
            problems.append('fallback before actual WSI destruction')
    if expected['mode'] in ('held', 'held-reuse'):
        kinds = [e['ev'] for e in relay_a['events']]
        if not all(k in kinds for k in ('client_hello', 'tls_hold', 'tls_release')):
            problems.append('missing held TLS stimulus')
        elif not kinds.index('client_hello') < kinds.index('tls_hold') < kinds.index('tls_release'):
            problems.append('TLS stimulus order')
        if result['gate'].get('ready_after_hold') is not True or result['gate'].get('release_after_child') is not True:
            problems.append('parent gate order')
        before, after = notes(rows, 'ssl-before-route'), notes(rows, 'ssl-after-route')
        release = [e['seq'] for e in rows if e['note'] == 'hold-release-request']
        if len(before) != 1 or len(after) != 1 or len(release) != 1 or not before[0][0] < after[0][0] < release[0]:
            problems.append('SSL hold witnesses')
        else:
            for _, witness in before + after:
                problems.extend(identity(witness, True, expected['provided'], trace['first_route']))
            keys = ('conn', 'wsi_token', 'ssl_token', 'route', 'actual_route')
            if before[0][1].get('conn') != trace['first_connection'] or any(before[0][1].get(k) != after[0][1].get(k) for k in keys):
                problems.append('held original identity changed')
            if expected['mode'] == 'held':
                late = [op for op in ops if op['route'] == trace['next_route']]
                for op in late:
                    if op['status'] != 'skipped' and not (before[0][0] < op.get('before', 0) < op.get('after', 0) < after[0][0]):
                        problems.append('late operation outside held SSL window')
                if len(late) != 2: problems.append('late create/init operations missing')
                for phase in ('create-before', 'create-after', 'tls-init-before', 'tls-init-after'):
                    for _, value in notes(rows, phase):
                        if value.get('route') != trace['next_route']: continue
                        watch = value.get('watch')
                        if not isinstance(watch, dict): problems.append('operation live watch absent'); continue
                        problems.extend(identity(watch, True, expected['provided'], trace['first_route']))
                        if any(watch.get(k) != before[0][1].get(k) for k in keys): problems.append('operation watched SSL differs')
            else:
                conn = trace['neighbor_connection']
                problems.extend(connection_proof(rows, conn, trace['next_route'], 'neighbor-ssl-open', True, expected['provided']))
                problems.extend(peer_proof(result, relay_b, neighbor, result['peer_b'], True))
                related = [e for e in rows if e['conn'] == conn]
                received = [e for e in related if e['name'] == 'RECEIVE']
                echoed = [e for e in related if e['note'] == 'neighbor-echo-held']
                payload = b'neighbor-route-echo'
                encoded = base64.b64encode(payload).decode('ascii')
                server_message = any(e.get('ev') == 'message' and e.get('kind') == 'text'
                                     and e.get('b64') == encoded and e.get('len') == len(payload) for e in neighbor['events'])
                server_echo = any(e.get('ev') == 'sent' and e.get('op') == 1 and e.get('fin') == 1
                                  and e.get('b64') == encoded and e.get('len') == len(payload) for e in neighbor['events'])
                if (len(received) != 1 or received[0]['hex'] != payload.hex() or received[0]['len'] != len(payload)
                        or len(echoed) != 1 or echoed[0]['len'] != 1 or not server_message or not server_echo
                        or not before[0][0] < received[0]['seq'] < echoed[0]['seq'] < after[0][0]):
                    problems.append('independent neighbor echo absent/outside hold')
                ends = [e for e in related if e['name'] == 'WSI_DESTROY']
                opens = [e for e in related if e['name'] == 'ESTABLISHED']
                marker = [e for e in related if e['note'] == 'neighbor-retired-held']
                tokens = {e['wsi'] for e in related if e['wsi'] > 0}
                all_ends = [e for e in rows if e['wsi'] in tokens and e['name'] == 'WSI_DESTROY']
                if not opens or not ends or not marker or marker[0]['len'] != 1 or not all_ends or any(not before[0][0] < e['seq'] < after[0][0] for e in related + all_ends):
                    problems.append('neighbor outside original held window')
                for phase in ('neighbor-ssl-open','verify-identity'):
                    for seq,value in notes(rows,phase):
                        if value.get('conn')==conn and not before[0][0] < seq < after[0][0]:
                            problems.append('neighbor identity outside held window')
    if expected['mode']=='destroy-active':
        beginnings, endings=notes(rows,'destroy-before'),notes(rows,'destroy-after')
        if len(beginnings)!=1 or len(endings)!=1 or beginnings[0][0]>=endings[0][0]:
            problems.append('active vhost destruction stages absent')
        stages=retirement_stages(rows,trace['final_connection'])
        if trace['opened'] and (not stages or any(v['stage']=='before destroy request' for v in stages)):
            problems.append('observed: active teardown had no live target token')
        derived=bool(stages) and all(v['stage']!='only during context shutdown' and v['stage']!='missing/duplicate' for v in stages)
        if stages and trace['retired_before_shutdown'] != derived:
            problems.append('retirement marker disagrees with token stages')
    return problems

def summarize(trace):
    if trace is None: return dict(outcome='partial/crashed; inspect flushed phases', operations=[], ownership='unestablished')
    rows = typed_events(trace)
    ops, errors = operations(rows)
    evidence = provenance(rows, trace)
    owner_errors, label = ownership(rows, trace, evidence)
    return dict(outcome='open+echo' if trace['opened'] and trace['echoed'] else 'did not complete open+echo',
                operations=ops, operation_errors=errors, ownership=label, ownership_errors=owner_errors,
                provenance_errors=evidence['errors'],
                retirements=retirement_stages(rows, trace['final_connection']))

def load_case(folder):
    result = read_object(folder / 'result.json'); immutable(result)
    hashes = result.get('sha256')
    if not isinstance(hashes, dict) or not hashes: raise ValueError('artifact manifest absent')
    required = {'unsupported.json'} if result.get('unsupported_ipv6') is True else {'runner.log', 'editor.log', 'acquisition.json', 'proxy-a.json', 'proxy-b.json', 'source.json', 'target.json', 'neighbor.json'}
    if (folder / 'trace.json').exists(): required.add('trace.json')
    if not required.issubset(hashes): raise ValueError('required artifact missing from manifest')
    for name, digest in hashes.items():
        if pathlib.Path(name).name != name or not isinstance(digest, str): raise ValueError('unsafe artifact path/hash')
        if hashlib.sha256((folder / name).read_bytes()).hexdigest() != digest: raise ValueError('artifact hash mismatch')
    if result.get('unsupported_ipv6') is True:
        unsupported(result, read_object(folder / 'unsupported.json'))
        return result, None, [], dict(outcome='IPv6 capability unavailable', operations=[], ownership='unestablished')
    trace = read_object(folder / 'trace.json') if (folder / 'trace.json').exists() else None
    artifacts = [read_object(folder / name) for name in ('proxy-a.json', 'proxy-b.json', 'source.json', 'target.json', 'neighbor.json')]
    errors = validate_case(result, trace, *artifacts)
    try: summary = summarize(trace)
    except (ValueError, KeyError, TypeError) as error: summary = dict(outcome='malformed trace', error=str(error), operations=[], ownership='unestablished')
    return result, trace, errors, summary

def render(directory, output):
    entries = read_object(directory / 'matrix.json').get('cases')
    if not isinstance(entries, list) or len(entries) != len(CASES): raise ValueError('complete immutable matrix required')
    if {e.get('case') for e in entries if isinstance(e, dict)} != {c[0] for c in CASES}: raise ValueError('matrix identity')
    lines = ['# LWS 3.0 immutable route probe — measured evidence', '',
             'Linux modular editor only. Acquisition completeness, observed differences and production suitability are separate.', '']
    for entry in entries:
        immutable(entry)
        result, trace, errors, summary = load_case(directory / entry['case'])
        if result != entry: raise ValueError('matrix/result disagreement')
        acquisition_errors = [e for e in errors if not e.startswith('observed:')]
        lines += ['## ' + entry['case'], '', 'Acquisition: ' + ('FAILED' if acquisition_errors else 'complete'), '',
                  '```json', json.dumps(dict(validation_errors=errors, summary=summary), sort_keys=True), '```', '']
    lines += ['No production route approval is inferred. Missing private index is unavailable, not unchanged.',
              'Every create/init operation has its own result and index pair; earlier vhosts cannot certify a late operation.',
              'Final SSL_CTX destruction is distinguished from SSL_CTX_free reference decrements.',
              'Root adjudicates negative, unsafe, crashed and unsupported results before B9. W3 and unload-worker obligations remain open.']
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile('w', encoding='utf-8', dir=output.parent, delete=False) as stream:
        temporary = pathlib.Path(stream.name); stream.write('\n'.join(lines) + '\n')
    try: os.replace(temporary, output)
    finally: temporary.unlink(missing_ok=True)

def synthetic(mode='held-reuse', provided=True):
    result = definition(mode + '-wss-' + ('provided' if provided else 'generated'))
    result.update(group_drained=True, runner_rc=0, gate=dict(ready_after_hold=True, release_after_child=True), errors=[],
                  target_port=443, peer_a=dict(host='127.0.0.1', port=5001), peer_b=dict(host='127.0.0.1', port=5002))
    result['journal_order'] = dict(child_group_drained=True, relays_naturally_idle=[True, True], before_fixture_stop=True)
    rows = []
    def append(name='marker', conn=0, wsi=0, note='', **extra):
        row = dict(seq=len(rows)+1, conn=conn, wsi=wsi, reason=0, name=name, time=0, len=0, input_present=False,
                   hex='', note=note, depth=-1, x509_error=-1, preverify=-1, flags=0)
        row.update(extra); rows.append(row)
    def ident(conn, route):
        return dict(conn=conn, route=route, actual_route=route, wsi_token=conn+6, ssl_token=conn,
                    ssl_exists=True, wsi_user=True, ctx_exists=True, ctx_supplied=provided,
                    ctx_origin='supplied' if provided else 'other', index_known=True, index_matches_wsi=True,
                    index=dict(known=True, value=5))
    def note(phase, **fields): append(conn=fields.get('conn', 0), note=json.dumps(dict(phase=phase, **fields)))
    def route(number, watch):
        for phase in ('create-before', 'create-after', 'tls-init-before', 'tls-init-after'):
            note(phase, route=number, value=True, index=dict(known=True, value=5), watch=watch)
    empty_watch = dict(conn=0, wsi_token=0, ssl_token=0, ssl_exists=False)
    route(1, empty_watch)
    if mode == 'held-reuse': route(2, empty_watch)
    append('WSI_CREATE', 1, 7); append('connect>', 1)
    note('ssl-before-route', **ident(1, 1))
    if mode == 'held': route(2, ident(1, 1))
    else:
        append('WSI_CREATE', 2, 8); append('connect>', 2)
        append('PERFORM_SERVER_CERT_VERIFICATION', 2, 8, depth=0, preverify=1, x509_error=0)
        note('verify-identity', **ident(2, 2), known_wsi=True, ssl_input=True, store_ssl=True, preverify=1)
        append('ESTABLISHED', 2, 8); note('neighbor-ssl-open', **ident(2, 2))
        append(conn=2, note='neighbor-open-held', len=1)
        append('RECEIVE', 2, 8, hex=b'neighbor-route-echo'.hex(), len=19)
        append(conn=2, note='neighbor-echo-held', len=1)
        append('WSI_DESTROY', 2, 8); append(conn=2, note='neighbor-retired-held', len=1)
    note('ssl-after-route', **ident(1, 1)); append(note='hold-release-request')
    append('PERFORM_SERVER_CERT_VERIFICATION', 1, 7, depth=0, preverify=1, x509_error=0)
    note('verify-identity', **ident(1, 1), known_wsi=True, ssl_input=True, store_ssl=True, preverify=1)
    append('ESTABLISHED', 1, 7); note('ssl-open', **ident(1, 1))
    append('WSI_DESTROY', 1, 7)
    labels=['cleanup-start','vhost-skipped-before','vhost-skipped-after','retirement-before','retirement-after',
            'shutdown-before','shutdown-after','caller-before','caller-after']
    for stage in range(9):
        if stage == 6: append(note='shutdown-end')
        note('ownership', stage=stage, label=labels[stage], frees=int(provided and stage == 8))
        if stage == 5: append(note='shutdown-begin')
    trace = dict(schema=1, case=result['case'], mode=mode, tls=True, provided=provided, events=rows, first_route=1, next_route=2,
                 first_connection=1, final_connection=1, neighbor_connection=2 if mode == 'held-reuse' else 0, selected_route=1,
                 harness_ok=True, first_primary=True, final_primary=True, opened=True, echoed=True, retired_before_fallback=False,
                 retired_before_shutdown=True, shutdown_completed=True, dropped=0, frees_after_context=0,
                 frees_after_caller=int(provided), final_ctx_destructions=[dict(stage=7, count=1)] if provided else [])
    def relay(port, held):
        events = [dict(ev='connect', headers=[], authority='localhost:443', peer_host='127.0.0.1', peer_port=port), dict(ev='client_hello', sni='localhost')]
        if held: events += [dict(ev='tls_hold'), dict(ev='tls_release')]
        return dict(events=events, dropped=0, active=0)
    journal = dict(events=[dict(ev='request', headers=[['Host','localhost:443'],['Authorization','Bearer route-probe-marker']])], dropped=0, connections=1)
    empty = dict(events=[], dropped=0, connections=0)
    neighbor = copy.deepcopy(journal) if mode == 'held-reuse' else copy.deepcopy(empty)
    if mode == 'held-reuse':
        payload = b'neighbor-route-echo'
        encoded = base64.b64encode(payload).decode('ascii')
        neighbor['events'] += [dict(ev='message', kind='text', len=len(payload), b64=encoded),
                               dict(ev='sent', op=1, fin=1, len=len(payload), b64=encoded)]
    return [result, trace, relay(5001, True), relay(5002, False) if mode == 'held-reuse' else dict(events=[], dropped=0, active=0), journal, empty, neighbor]

def change_note(values, phase, edit, first=True):
    for event in values[1]['events']:
        if event['note'].startswith('{'):
            value = json.loads(event['note'])
            if value.get('phase') == phase:
                edit(value); event['note'] = json.dumps(value)
                if first: return

def remove_phase(values, phase):
    values[1]['events'] = [e for e in values[1]['events'] if not e['note'].startswith('{') or json.loads(e['note']).get('phase') != phase]
    for number, event in enumerate(values[1]['events'], 1): event['seq'] = number

def synthetic_untrusted():
    values = synthetic('held')
    values[0].update(definition('untrusted-wss-provided'))
    values[1].update(case=values[0]['case'], mode='untrusted', opened=False, echoed=False)
    for phase in ('ssl-before-route', 'ssl-after-route', 'ssl-open'):
        remove_phase(values, phase)
    rows = values[1]['events']
    rows[:] = [e for e in rows if e['name'] != 'ESTABLISHED' and e['note'] != 'hold-release-request']
    for event in rows:
        if event['name'] == 'PERFORM_SERVER_CERT_VERIFICATION':
            event.update(preverify=0, x509_error=18)
        if event['note'].startswith('{'):
            value = json.loads(event['note'])
            if value.get('phase') == 'verify-identity': value['preverify'] = 0
            if 'watch' in value: value['watch'] = dict(conn=0, wsi_token=0, ssl_token=0, ssl_exists=False)
            event['note'] = json.dumps(value)
    for n, event in enumerate(rows, 1): event['seq'] = n
    values[2]['events'] = [e for e in values[2]['events'] if e['ev'] not in ('tls_hold', 'tls_release')]
    values[4] = dict(events=[], dropped=0, connections=0)
    return values

class ValidatorTest(unittest.TestCase):
    def test_peer_reset_requires_same_connection_prior_hello(self):
        original = synthetic_untrusted()
        hello = next(e for e in original[2]['events'] if e['ev'] == 'client_hello')
        hello.update(id=1, t=1.0, record_bytes=128)
        original[2]['events'].append(dict(ev='peer_reset', id=1, t=2.0,
            operation='recv-client', errno=errno.ECONNRESET, phase='after-forwarded-client-hello'))
        self.assertEqual(validate_case(*original), [])
        for mutation in ('missing-hello', 'wrong-id', 'after-reset', 'bad-record', 'bad-sni',
                         'bad-id', 'bad-time', 'bad-operation', 'bad-errno', 'bad-phase'):
            values = copy.deepcopy(original); events = values[2]['events']
            hello = next(e for e in events if e['ev'] == 'client_hello'); reset = events[-1]
            if mutation == 'missing-hello': events.remove(hello)
            elif mutation == 'wrong-id': hello['id'] = 2
            elif mutation == 'after-reset': events.remove(hello); events.append(hello)
            elif mutation == 'bad-record': hello['record_bytes'] = 0
            elif mutation == 'bad-sni': hello['sni'] = None
            elif mutation == 'bad-id': reset['id'] = True
            elif mutation == 'bad-time': reset['t'] = 0.5
            elif mutation == 'bad-operation': reset['operation'] = 'recv-upstream'
            elif mutation == 'bad-errno': reset['errno'] = errno.EMFILE
            elif mutation == 'bad-phase': reset['phase'] = 'before-hello'
            self.assertIn('peer reset provenance', validate_case(*values), mutation)
        for changed in (dict(errors=['fixture cleanup: failed']), dict(abort_matrix=True)):
            value = definition('ipv6-bare-port-wss')
            evidence = dict(operation='bind-ipv6-loopback', family='AF_INET6', host='::1', errno=errno.EAFNOSUPPORT)
            value.update(unsupported_ipv6=True, errno=evidence['errno'], group_drained=True, errors=[])
            value.update(changed)
            with self.assertRaises(ValueError): unsupported(value, evidence)

    def test_neighbor_echo_is_actual_and_inside_window(self):
        original = synthetic()
        self.assertEqual(validate_case(*original), [])
        for change in ('missing', 'payload', 'after-window', 'marker', 'server-message', 'server-echo'):
            values = copy.deepcopy(original)
            rows = values[1]['events']
            received = next(e for e in rows if e['conn'] == 2 and e['name'] == 'RECEIVE')
            if change == 'missing': rows.remove(received)
            elif change == 'payload': received['hex'] = b'wrong'.hex()
            elif change == 'after-window': rows.remove(received); rows.append(received)
            elif change == 'marker': next(e for e in rows if e['note'] == 'neighbor-echo-held')['len'] = 0
            else:
                kind = 'message' if change == 'server-message' else 'sent'
                values[6]['events'] = [e for e in values[6]['events'] if e['ev'] != kind]
            for n,e in enumerate(rows,1): e['seq'] = n
            self.assertIn('independent neighbor echo absent/outside hold', validate_case(*values), change)
        self.assertEqual(validate_case(*original), [])

    def test_authoritative_journals_precede_cleanup(self):
        for changed in ({}, dict(child_group_drained=False, relays_naturally_idle=[True, True], before_fixture_stop=True),
                        dict(child_group_drained=True, relays_naturally_idle=[True, False], before_fixture_stop=True),
                        dict(child_group_drained=True, relays_naturally_idle=[True, True], before_fixture_stop=False)):
            values = synthetic(); values[0]['journal_order'] = changed
            self.assertIn('authoritative relay ordering unproven', validate_case(*values))
    def test_valid_original_and_neighbor(self):
        for mode in ('held', 'held-reuse'):
            self.assertEqual(validate_case(*synthetic(mode)), [])
    def test_neighbor_requires_own_every_witness(self):
        for phase in ('neighbor-ssl-open', 'verify-identity'):
            values = synthetic(); remove_phase(values, phase)
            self.assertTrue(validate_case(*values))
        for field, changed in (('conn',999), ('actual_route',1), ('ssl_token',99), ('ctx_supplied',False)):
            values = synthetic(); change_note(values, 'neighbor-ssl-open', lambda v: v.update({field:changed}))
            self.assertTrue(validate_case(*values))
        for index in (3, 6):
            values = synthetic(); values[index]['events'] = []
            self.assertTrue(validate_case(*values))
        values = synthetic(); values[3]['events'][0]['peer_port'] = 5001
        self.assertTrue(validate_case(*values))
        values = synthetic(); rows=values[1]['events']; opened=next(e for e in rows if e['conn']==2 and e['name']=='ESTABLISHED')
        rows.remove(opened); rows.append(opened)
        for n,e in enumerate(rows,1): e['seq']=n
        self.assertIn('neighbor outside original held window', validate_case(*values))
    def test_operation_pairs_and_original_identity(self):
        for phase in ('create-after','tls-init-before','tls-init-after'):
            values=synthetic('held'); remove_phase(values,phase)
            self.assertTrue(validate_case(*values))
        values=synthetic('held'); change_note(values,'ssl-after-route',lambda v:v.update(ssl_token=77))
        self.assertIn('held original identity changed',validate_case(*values))
        values=synthetic('held'); change_note(values,'tls-init-before',lambda v:v.update(index=dict(known=True)),first=False)
        self.assertIn('known index value type',validate_case(*values))
        for action in ('omit','change'):
            values=synthetic('held')
            for e in values[1]['events']:
                if not e['note'].startswith('{'): continue
                v=json.loads(e['note'])
                if v.get('phase')=='tls-init-before' and v.get('route')==2:
                    if action=='omit': v.pop('watch')
                    else: v['watch']['ssl_token']=999
                    e['note']=json.dumps(v)
            self.assertIn('operation live watch absent' if action=='omit' else 'operation watched SSL differs',validate_case(*values))
    def test_index_motion_attributed_only_to_own_pair(self):
        values=synthetic('held'); change_note(values,'create-after',lambda v:v.update(index=dict(known=True,value=9)))
        ops,_=operations(values[1]['events'])
        self.assertEqual(next(o for o in ops if o['route']==1 and o['operation']=='create')['motion'],'changed')
        self.assertTrue(all(o['motion']=='unchanged' for o in ops if o['route']==2))
        for operation in ('create','tls-init'):
            values=synthetic('held')
            for e in values[1]['events']:
                if e['note'].startswith('{'):
                    v=json.loads(e['note'])
                    if v.get('phase')==operation+'-after' and v.get('route')==2:
                        v['index']['value']=6; e['note']=json.dumps(v)
            ops,_=operations(values[1]['events'])
            self.assertEqual(next(o for o in ops if o['route']==2 and o['operation']==operation)['motion'],'changed')
        values=synthetic('held')
        for phase in ('create-before','create-after','tls-init-before','tls-init-after'):
            change_note(values,phase,lambda v:v.update(index=dict(known=False)),first=False)
        self.assertTrue(all(o['motion']=='unavailable' for o in operations(values[1]['events'])[0]))
    def test_fixture_failure_and_missing_untrusted_stimulus(self):
        values=synthetic(); values[2]['events'].append(dict(ev='fixture_error',kind='ConnectionRefusedError'))
        values[1].update(opened=False,echoed=False)
        self.assertIn('fixture stimulus failed',validate_case(*values))
        for ev in ('hold_deadline','hold_stopped'):
            values=synthetic(); values[3]['events'].append(dict(ev=ev))
            self.assertIn('fixture stimulus failed',validate_case(*values))
        values=synthetic(); values[0].update(definition('untrusted-wss-provided')); values[1].update(case=values[0]['case'],mode='untrusted',opened=False,echoed=False)
        self.assertIn('untrusted failed-certificate stimulus absent',validate_case(*values))
    def test_context_provenance_is_not_free_count(self):
        for factory in (synthetic, synthetic_untrusted):
            original = factory()
            self.assertEqual(validate_case(*original), [])
            self.assertEqual(summarize(original[1])['ownership'], 'exercised supplied context finally destroyed by caller')
            phase = 'verify-identity' if factory == synthetic_untrusted else 'ssl-open'
            for change in ('omit', 'malformed', 'wrong-context', 'missing-context', 'wrong-token', 'wrong-callback', 'omit-record'):
                values = copy.deepcopy(original)
                def edit(value):
                    if change == 'omit': value.pop('ctx_origin')
                    elif change == 'malformed': value['ctx_origin'] = 'forged'
                    elif change == 'wrong-context': value.update(ctx_supplied=False, ctx_origin='other')
                    elif change == 'missing-context': value['ctx_exists'] = False
                    elif change == 'wrong-token': value['wsi_token'] = 999
                    elif change == 'wrong-callback': value['preverify'] = 1
                if change == 'omit-record': remove_phase(values, phase)
                elif change == 'wrong-callback' and factory != synthetic_untrusted:
                    change_note(values, phase, lambda v: v.update(conn=999))
                else: change_note(values, phase, edit)
                self.assertTrue(validate_case(*values), (factory.__name__, change))
                self.assertEqual(summarize(values[1])['ownership'], 'supplied use unestablished', (factory.__name__, change))
                # Exact restoration is a positive control; no archive is rewritten during actual acquisition.
                values = copy.deepcopy(original)
                self.assertEqual(validate_case(*values), [])
                self.assertEqual(summarize(values[1])['ownership'], 'exercised supplied context finally destroyed by caller')
        values = synthetic_untrusted(); remove_phase(values, 'verify-identity')
        self.assertIn('certificate callback requires one associated provenance record', validate_case(*values))
        self.assertEqual(summarize(values[1])['ownership'], 'supplied use unestablished')
    def test_actual_token_lifetime_not_coherent_note_numbers(self):
        values = synthetic('held')
        for event in values[1]['events']:
            if not event['note'].startswith('{'): continue
            value = json.loads(event['note'])
            if value.get('conn') == 1 and 'wsi_token' in value: value['wsi_token'] = 999
            if isinstance(value.get('watch'), dict) and value['watch'].get('conn') == 1:
                value['watch']['wsi_token'] = 999
            event['note'] = json.dumps(value)
        self.assertIn('identity token not associated with connection', validate_case(*values))
        self.assertEqual(summarize(values[1])['ownership'], 'supplied use unestablished')
        for where in ('before', 'during-create', 'during-init'):
            values = synthetic('held'); rows = values[1]['events']
            end = next(e for e in rows if e['name'] == 'WSI_DESTROY' and e['wsi'] == 7)
            rows.remove(end)
            phase = {'before':'ssl-before-route', 'during-create':'create-after', 'during-init':'tls-init-after'}[where]
            index = next(i for i,e in enumerate(rows) if e['note'].startswith('{')
                         and json.loads(e['note']).get('phase') == phase
                         and (where == 'before' or json.loads(e['note']).get('route') == 2))
            rows.insert(index, end)
            for n,e in enumerate(rows,1): e['seq'] = n
            self.assertTrue(validate_case(*values), where)
            self.assertEqual(summarize(values[1])['ownership'], 'supplied use unestablished')
        for phase in ('ssl-open', 'verify-identity'):
            values = synthetic('held'); rows = values[1]['events']
            name = 'ESTABLISHED' if phase == 'ssl-open' else 'PERFORM_SERVER_CERT_VERIFICATION'
            next(e for e in rows if e['name'] == name)['wsi'] = 999
            self.assertTrue(validate_case(*values), phase)
            self.assertEqual(summarize(values[1])['ownership'], 'supplied use unestablished')
    def test_conn_zero_create_and_repeated_create_remain_valid(self):
        for repeat in (False, True):
            values = synthetic('held'); rows = values[1]['events']
            created = next(e for e in rows if e['name'] == 'WSI_CREATE')
            created['conn'] = 0
            if repeat:
                index = rows.index(created)
                repeated = copy.deepcopy(created); repeated['conn'] = 1
                rows.insert(index+1, repeated)
            # Without repeated CREATE, the certificate callback later supplies independent conn1 association.
            # Its token is stable from conn0 CREATE through DESTROY; notes cannot invent that association.
            for n,e in enumerate(rows,1): e['seq'] = n
            self.assertEqual(validate_case(*values), [])
            self.assertEqual(summarize(values[1])['ownership'], 'exercised supplied context finally destroyed by caller')
    def test_all_emitted_fixture_cutoffs_invalidate_every_relay(self):
        for factory in (lambda: synthetic('held'), synthetic, synthetic_untrusted):
            original = factory()
            self.assertEqual(validate_case(*original), [])
            # The ungated negative row already has real CONNECT, ClientHello and failed verification.
            for relay in (2, 3):
                for name in ('fixture_error', 'hold_deadline', 'hold_stopped', 'stopped', 'relay_deadline'):
                    values = copy.deepcopy(original)
                    values[relay]['events'].append(dict(ev=name))
                    self.assertIn('fixture stimulus failed', validate_case(*values), (factory, relay, name))
            self.assertEqual(validate_case(*original), [])
    def test_final_destruction_stage_chain(self):
        values=synthetic(); remove_phase(values,'ownership')
        self.assertIn('ownership stage chain',validate_case(*values))
        for stage,count in ((1,1),(5,1),(7,2)):
            values=synthetic(); values[1]['final_ctx_destructions']=[dict(stage=stage,count=n) for n in range(1,count+1)]
            values[1]['frees_after_context']=count if stage<6 else 0; values[1]['frees_after_caller']=count
            for e in values[1]['events']:
                if e['note'].startswith('{'):
                    v=json.loads(e['note'])
                    if v.get('phase')=='ownership': v['frees']=count if v['stage']>stage else 0; e['note']=json.dumps(v)
            self.assertEqual(validate_case(*values),[])
            self.assertNotEqual(summarize(values[1])['ownership'],'exercised supplied context finally destroyed by caller')
        values=synthetic(); values[1]['final_ctx_destructions']=[]; values[1]['frees_after_caller']=0
        change_note(values,'ownership',lambda v:v.update(frees=0),first=False)
        self.assertEqual(summarize(values[1])['ownership'],'missing final destruction')
    def test_unsupported_requires_immutable_capability(self):
        evidence=dict(operation='bind-ipv6-loopback',family='AF_INET6',host='::1',errno=errno.EAFNOSUPPORT)
        value=definition('ipv6-bare-port-wss'); value.update(unsupported_ipv6=True,errno=evidence['errno'],group_drained=True)
        unsupported(value,evidence)
        for changed in (dict(case='precreated-wss-generated',mode='precreated',tls=True,provided=False,form='v4'),dict(tls='yes'),dict(mode='held')):
            altered=copy.deepcopy(value); altered.update(changed)
            with self.assertRaises(ValueError): unsupported(altered,evidence)
        altered=copy.deepcopy(evidence); altered['errno']=errno.EMFILE
        value['errno']=errno.EMFILE
        with self.assertRaises(ValueError): unsupported(value,altered)
    def test_lifecycle_and_type_and_output_refusals(self):
        for mutate in ('missing','double','badtype','loss'):
            values=synthetic(); rows=values[1]['events']
            end=next(e for e in rows if e['name']=='WSI_DESTROY')
            if mutate=='missing': rows.remove(end)
            elif mutate=='double': rows.append(copy.deepcopy(end))
            elif mutate=='badtype': rows[0]['wsi']='7'
            else: values[1]['dropped']=1
            for n,e in enumerate(rows,1): e['seq']=n
            self.assertTrue(validate_case(*values))
        with tempfile.TemporaryDirectory() as directory:
            root=pathlib.Path(directory); (root/'matrix.json').write_text(json.dumps(dict(cases=[])))
            output=root/'passport.md'; output.write_text('accepted\n')
            with self.assertRaises(ValueError): render(root,output)
            self.assertEqual(output.read_text(),'accepted\n')
    def test_active_retirement_is_not_context_cleanup(self):
        values=synthetic('held'); values[0].update(definition('destroy-active-wss-provided'))
        values[1].update(case=values[0]['case'],mode='destroy-active')
        rows=values[1]['events']; end=next(e for e in rows if e['conn']==1 and e['name']=='WSI_DESTROY')
        rows.remove(end)
        for event in rows:
            if event['note'].startswith('{'):
                v=json.loads(event['note'])
                if v.get('phase')=='ownership' and v.get('stage') in (1,2):
                    v['label']='vhost-before' if v['stage']==1 else 'vhost-after'; event['note']=json.dumps(v)
        for phase,stage in (('destroy-before',1),('destroy-after',3)):
            index=next(i for i,e in enumerate(rows) if e['note'].startswith('{') and json.loads(e['note']).get('phase')=='ownership' and json.loads(e['note']).get('stage')==stage)
            event=copy.deepcopy(end); event.update(conn=0,wsi=0,name='marker',note=json.dumps(dict(phase=phase,route=1,value=True,index=dict(known=False))))
            rows.insert(index,event)
        index=next(i for i,e in enumerate(rows) if e['note'].startswith('{') and json.loads(e['note']).get('phase')=='ownership' and json.loads(e['note']).get('stage')==4)
        rows.insert(index,end)
        for n,e in enumerate(rows,1): e['seq']=n
        self.assertEqual(validate_case(*values),[])
        self.assertEqual(retirement_stages(rows,1)[0]['stage'],'during retirement wait')
        rows.remove(end); rows.append(end); values[1]['retired_before_shutdown']=False
        for n,e in enumerate(rows,1): e['seq']=n
        self.assertEqual(validate_case(*values),[])
        self.assertEqual(retirement_stages(rows,1)[0]['stage'],'only during context shutdown')
    def test_unsupported_render_refusal_preserves_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root=pathlib.Path(directory); entries=[definition(c[0]) for c in CASES]
            bad=entries[0]; bad.update(unsupported_ipv6=True,group_drained=True,errno=errno.EAFNOSUPPORT)
            folder=root/bad['case']; folder.mkdir()
            evidence=dict(operation='bind-ipv6-loopback',family='AF_INET6',host='::1',errno=errno.EAFNOSUPPORT)
            data=json.dumps(evidence).encode(); (folder/'unsupported.json').write_bytes(data)
            bad['sha256']={'unsupported.json':hashlib.sha256(data).hexdigest()}
            (folder/'result.json').write_text(json.dumps(bad)); (root/'matrix.json').write_text(json.dumps(dict(cases=entries)))
            output=root/'passport.md'; output.write_text('accepted\n')
            with self.assertRaises(ValueError): render(root,output)
            self.assertEqual(output.read_text(),'accepted\n')

def main():
    parser=argparse.ArgumentParser(); parser.add_argument('--selftest',action='store_true')
    parser.add_argument('--run',type=pathlib.Path); parser.add_argument('--out',type=pathlib.Path)
    parser.add_argument('--case',type=pathlib.Path); parser.add_argument('--require-positive',action='store_true')
    args=parser.parse_args()
    if args.selftest:
        return int(not unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ValidatorTest)).wasSuccessful())
    if args.case:
        result,trace,errors,summary=load_case(args.case)
        if args.require_positive and (trace is None or not trace.get('opened') or not trace.get('echoed')):
            errors.append('required positive route did not open+echo')
        print(json.dumps(dict(case=result['case'],errors=errors,summary=summary),indent=2))
        return int(bool(errors))
    if not args.run or not args.out: parser.error('--run and --out required')
    render(args.run,args.out); return 0
if __name__=='__main__': raise SystemExit(main())
