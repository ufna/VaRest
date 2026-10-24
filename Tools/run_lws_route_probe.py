#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
import argparse
import unittest
import contextlib
import errno
import hashlib
import json
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.parse
import uuid
sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from run_lws_proxy_probe import ROOT, target_server, child_environment, direct_json, stop_group
from lws_route_passport import CASES, definition
sys.path.insert(0, str(ROOT / 'Tools/testserver'))
from route_proxy import RouteProxy, Ipv6Unavailable

def save(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')

def manifest(folder):
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.iterdir()
            if p.is_file() and p.name != 'result.json'}

def snapshot_stimulus(folder, result, proxies):
    # Wait for both independently: a false result must not short-circuit the second observation.
    idle = [proxy.wait_idle(2) for proxy in proxies]
    result['journal_order'] = dict(child_group_drained=result.get('group_drained') is True,
                                  relays_naturally_idle=idle,
                                  before_fixture_stop=not any(proxy.stopping.is_set() for proxy in proxies))
    if len(proxies) != 2 or not all(idle):
        result['errors'].append('relay not naturally idle before fixture shutdown')
    for name, proxy in zip(('proxy-a.json', 'proxy-b.json'), proxies):
        save(folder / name, proxy.snapshot())
    save(folder / 'result.partial.json', result)

@contextlib.contextmanager
def fixture_scope(folder, result, proxies):
    stack = contextlib.ExitStack()
    try:
        yield stack
    except Exception as error:
        result['errors'].append('acquisition: ' + type(error).__name__ + ': ' + str(error))
        result['abort_matrix'] = True
    finally:
        # Preserve the actual pre-stop state even if acquisition raised before normal sampling.
        for name, proxy in zip(('proxy-a.json', 'proxy-b.json'), proxies):
            if not (folder / name).exists():
                save(folder / name, proxy.snapshot())
        save(folder / 'result.partial.json', result)
        try:
            stack.close()
        except Exception as error:
            result['errors'].append('fixture cleanup: ' + type(error).__name__ + ': ' + str(error))
            result['abort_matrix'] = True
        for name, proxy in zip(('proxy-a-after-cleanup.json', 'proxy-b-after-cleanup.json'), proxies):
            save(folder / name, proxy.snapshot())
        save(folder / 'result.partial.json', result)

def observe_child(process, folder, proxy, held, ready, release, result, environment):
    result['child_pid'] = result['child_pgid'] = getattr(process, 'pid', None)
    result['child_started_unix'] = time.time()
    began = time.monotonic()
    limit = time.monotonic() + 120
    try:
        while process.poll() is None and time.monotonic() < limit:
            if held and proxy.server_held.is_set() and not ready.exists():
                save(folder / 'proxy-held.json', proxy.snapshot())
                ready.write_text('actual-server-flight-held\n')
                result['gate']['ready_after_hold'] = True
            if held and release.exists() and not proxy.release_tls.is_set():
                result['gate']['release_after_child'] = ready.exists() and proxy.server_held.is_set()
                save(folder / 'gate-order.json', result['gate'])
                proxy.release_tls.set()
            threading.Event().wait(.02)
        result['runner_rc'] = process.wait(timeout=1) if process.poll() is not None else 124
    finally:
        try:
            stop_group(process)
            result['group_drained'] = True
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
            result['errors'].append('cleanup: ' + str(error))
            result['abort_matrix'] = True
        editor = pathlib.Path(environment.get('VAREST_HOST', '/w/Unreal/VaRestHost')) / 'Saved/Logs/VaRestTests.log'
        if editor.exists():
            shutil.copyfile(editor, folder / 'editor.log')
        else:
            result['errors'].append('editor log missing')
            (folder / 'editor.log').write_text('No editor log was available for this child.\n')
        result['child_duration_seconds'] = time.monotonic() - began
        save(folder / 'child-status.json', result)

class ControllerTest(unittest.TestCase):
    def test_real_client_reset_after_forwarded_hello(self):
        from route_proxy_selftest import target, connect, hello
        with target() as port, RouteProxy([port], inspect_tls=True) as proxy:
            sock = connect(proxy, port)
            try:
                payload = hello(); sock.sendall(payload); received = bytearray()
                while len(received) < len(payload):
                    chunk = sock.recv(len(payload) - len(received))
                    self.assertTrue(chunk, 'relay ended before complete echo')
                    received.extend(chunk)
                self.assertEqual(bytes(received), payload)
                self.assertTrue(proxy.hello_seen.is_set())
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
            finally: sock.close()
            self.assertTrue(proxy.wait_idle(2))
            events = proxy.snapshot()['events']
            self.assertFalse(proxy.stopping.is_set())
            resets = [e for e in events if e['ev'] == 'peer_reset']
            self.assertEqual(len(resets), 1, events)
            self.assertEqual(resets[0]['operation'], 'recv-client')
            self.assertEqual(resets[0]['errno'], errno.ECONNRESET)
            self.assertEqual(resets[0]['phase'], 'after-forwarded-client-hello')
            hello_event = next(e for e in events if e['ev'] == 'client_hello')
            self.assertEqual(hello_event['id'], resets[0]['id'])
            self.assertLess(hello_event['t'], resets[0]['t'])
            self.assertFalse(any(e['ev'] in ('fixture_error', 'tls_release', 'stopped', 'relay_deadline') for e in events))

    def test_reset_before_hello_and_broken_upstream_remain_failures(self):
        from route_proxy_selftest import target, connect
        with target() as port, RouteProxy([port], inspect_tls=True) as proxy:
            sock = connect(proxy, port)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0)); sock.close()
            self.assertTrue(proxy.wait_idle(2))
            errors = [e for e in proxy.snapshot()['events'] if e['ev'] == 'fixture_error']
            self.assertTrue(errors); self.assertEqual(errors[0]['operation'], 'recv-client')
            self.assertNotIn('peer_reset', [e['ev'] for e in proxy.snapshot()['events']])
        with socket.socket() as refused:
            refused.bind(('127.0.0.1', 0)); port = refused.getsockname()[1]
            with RouteProxy([port], inspect_tls=True) as proxy:
                with self.assertRaises(AssertionError): connect(proxy, port)
                self.assertTrue(proxy.wait_idle(2))
                errors = [e for e in proxy.snapshot()['events'] if e['ev'] == 'fixture_error']
                self.assertTrue(errors); self.assertEqual(errors[0]['operation'], 'connect-upstream')
                self.assertNotIn('peer_reset', [e['ev'] for e in proxy.snapshot()['events']])

    def test_first_constructor_results_sealed_after_cleanup(self):
        import tempfile
        from unittest.mock import patch
        for capability, cleanup_failure in ((True, False), (False, False), (True, True)):
            with tempfile.TemporaryDirectory() as directory:
                folder = pathlib.Path(directory) / 'case'
                error = (Ipv6Unavailable(OSError(errno.EAFNOSUPPORT, 'synthetic IPv6 unavailable')) if capability
                         else OSError(errno.EMFILE, 'synthetic descriptor limit'))
                item = next(v for v in CASES if v[0] == 'ipv6-bare-port-wss')
                with patch(__name__ + '.RouteProxy', side_effect=error):
                    with patch('contextlib.ExitStack.close', side_effect=RuntimeError('synthetic cleanup failure')) if cleanup_failure else contextlib.nullcontext():
                        result = one(folder, item, 'http://127.0.0.1:40001', 'https://127.0.0.1:40002')
                self.assertEqual(result, json.loads((folder/'result.json').read_text()))
                self.assertEqual(result['sha256'], manifest(folder))
                self.assertEqual(set(result['sha256']), {p.name for p in folder.iterdir() if p.is_file() and p.name != 'result.json'})
                self.assertEqual(result.get('unsupported_ipv6') is True, capability)
                if cleanup_failure:
                    self.assertTrue(result['abort_matrix'])
                    self.assertTrue(any('fixture cleanup:' in e for e in result['errors']))
                    with self.assertRaises(ValueError): __import__('lws_route_passport').load_case(folder)
                elif not capability: self.assertTrue(result['errors'])

    def test_stimulus_snapshot_precedes_stop_and_preserves_post_cleanup(self):
        import tempfile
        class Relay:
            def __init__(self): self.stopping = threading.Event(); self.waits = 0
            def wait_idle(self, seconds): self.waits += 1; return True
            def snapshot(self): return dict(events=[dict(ev='stopped')] if self.stopping.is_set() else [], active=0, dropped=0)
        with tempfile.TemporaryDirectory() as directory:
            folder = pathlib.Path(directory); proxies = [Relay(), Relay()]
            result = dict(group_drained=True, errors=[])
            with fixture_scope(folder, result, proxies) as stack:
                for proxy in proxies: stack.callback(proxy.stopping.set)
                snapshot_stimulus(folder, result, proxies)
            self.assertEqual(result['journal_order'], dict(child_group_drained=True, relays_naturally_idle=[True, True], before_fixture_stop=True))
            for letter in ('a', 'b'):
                self.assertEqual(json.loads((folder/('proxy-'+letter+'.json')).read_text())['events'], [])
                self.assertEqual(json.loads((folder/('proxy-'+letter+'-after-cleanup.json')).read_text())['events'], [dict(ev='stopped')])
            self.assertEqual([p.waits for p in proxies], [1, 1])

    def test_fixture_cleanup_failure_preserves_journals_and_aborts(self):
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory); launches = []
            def fail(): raise RuntimeError('synthetic fixture drain refused')
            def acquire(folder, item):
                folder.mkdir(); launches.append(item[0]); result = dict(group_drained=True, runner_rc=73, errors=[])
                with fixture_scope(folder, result, []) as stack:
                    stack.callback(fail)
                    save(folder/'proxy-a.json', dict(events=[dict(ev='tls_hold')]))
                save(folder/'result.json', result)
                return result
            with self.assertRaises(AcquisitionAborted): acquire_matrix(root, [('first',), ('second',)], acquire)
            self.assertEqual(launches, ['first'])
            self.assertIn('tls_hold', (root/'first/proxy-a.json').read_text())
            self.assertTrue(json.loads((root/'first/result.json').read_text())['abort_matrix'])

    def test_idle_failure_checks_both_and_stays_partial(self):
        import tempfile
        from unittest.mock import Mock
        with tempfile.TemporaryDirectory() as directory:
            result = dict(group_drained=False, errors=[]); proxies = []
            for idle in (False, True):
                proxy = Mock(); proxy.wait_idle.return_value = idle; proxy.stopping.is_set.return_value = False
                proxy.snapshot.return_value = dict(events=[], active=0, dropped=0); proxies.append(proxy)
            snapshot_stimulus(pathlib.Path(directory), result, proxies)
            self.assertTrue(result['errors'])
            self.assertEqual(result['journal_order']['relays_naturally_idle'], [False, True])
            for proxy in proxies: proxy.wait_idle.assert_called_once_with(2)

    def test_failed_child_keeps_status_and_drains(self):
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            folder = pathlib.Path(directory)
            log = folder / 'Saved/Logs'
            log.mkdir(parents=True)
            (log / 'VaRestTests.log').write_text('PENDING synthetic-controller-exit\n')
            result = dict(runner_rc=-1, group_drained=False, gate={}, errors=[])
            with (folder / 'runner.log').open('wb') as output:
                process = subprocess.Popen([sys.executable, '-c', 'import sys;sys.exit(73)'],
                    stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
                observe_child(process, folder, None, False, folder / 'ready', folder / 'release',
                              result, {'VAREST_HOST': str(folder)})
            self.assertEqual(result['runner_rc'], 73)
            self.assertTrue(result['group_drained'])
            self.assertEqual(result['errors'], [])
            self.assertIn('PENDING synthetic-controller-exit', (folder / 'editor.log').read_text())
            self.assertFalse((folder / 'trace.json').exists())

    def test_failed_drain_preserves_first_and_never_launches_next(self):
        import tempfile
        from unittest.mock import patch
        class Exited:
            def poll(self): return 73
            def wait(self, timeout): return 73
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            launches = []
            def acquire(folder, item):
                launches.append(item[0]); folder.mkdir()
                result = dict(runner_rc=-1, group_drained=False, gate={}, errors=[])
                with patch(__name__ + '.stop_group', side_effect=RuntimeError('synthetic drain refused')):
                    observe_child(Exited(), folder, None, False, folder/'ready', folder/'release', result, {'VAREST_HOST': str(root)})
                save(folder/'proxy-a.json', dict(events=[dict(ev='tls_hold')]))
                save(folder/'result.json', result)
                return result
            with self.assertRaises(AcquisitionAborted):
                acquire_matrix(root, [('first',), ('second',)], acquire)
            self.assertEqual(launches, ['first'])
            self.assertEqual(json.loads((root/'first/result.json').read_text())['runner_rc'], 73)
            self.assertIn('tls_hold', (root/'first/proxy-a.json').read_text())
            self.assertEqual(len(json.loads((root/'matrix.json').read_text())['cases']), 1)

class AcquisitionAborted(RuntimeError):
    pass

def acquire_matrix(folder, items, acquire):
    results = []
    for item in items:
        result = acquire(folder / item[0], item)
        results.append(result)
        save(folder / 'matrix.json', dict(schema=1, baseline='f1a7bbf40774095547c8e58b00d7afcd5617c61c', cases=results))
        if result.get('group_drained') is not True or result.get('abort_matrix') is True:
            raise AcquisitionAborted('owned child group drain unproven; no next acquisition')
    return results

def one(folder, item, plain, tls):
    result = acquire_one(folder, item, plain, tls)
    # Every early return has left fixture_scope and persisted its final cleanup state.
    result['sha256'] = manifest(folder)
    save(folder / 'result.json', result)
    return result

def acquire_one(folder, item, plain, tls):
    ident, mode, use_tls, provided, form = item
    result = definition(ident)
    result.update(runner_rc=-1, group_drained=False, gate={}, errors=[])
    folder.mkdir()
    ports = [urllib.parse.urlsplit(plain).port, urllib.parse.urlsplit(tls).port]
    source_key, target_key = uuid.uuid4().hex, uuid.uuid4().hex
    path = '/ws/echo?key=' + source_key
    if mode == 'redirect':
        location = '%s://127.0.0.1:%d/ws/echo?key=%s' % ('wss' if use_tls else 'ws', ports[1 if use_tls else 0], target_key)
        path = '/ws/status/302?key=' + source_key + '&location=' + urllib.parse.quote(location, safe='')
    held = mode in ('held', 'held-reuse')
    proxies = []
    with fixture_scope(folder, result, proxies) as stack:
        try:
            proxy_a = stack.enter_context(RouteProxy(ports, family=socket.AF_INET if form == 'v4' else socket.AF_INET6,
                                                    hold_tls=held, inspect_tls=use_tls))
            proxies.append(proxy_a)
        except Ipv6Unavailable as error:
            result.update(unsupported_ipv6=True, errno=error.errno, detail=str(error), group_drained=True)
            save(folder / 'unsupported.json', error.evidence)
            return result
        except OSError as error:
            result.update(group_drained=True, target_port=ports[1 if use_tls else 0],
                          errors=['fixture construction: ' + type(error).__name__ + ': ' + str(error)])
            save(folder / 'acquisition.json', result)
            for name in ('proxy-a.json', 'proxy-b.json', 'source.json', 'target.json', 'neighbor.json'):
                save(folder / name, dict(acquisition_error=result['errors'][0]))
            for name in ('runner.log', 'editor.log'):
                (folder / name).write_text('No child started: fixture construction failed.\n')
            return result
        proxy_b = stack.enter_context(RouteProxy(ports, inspect_tls=use_tls))
        proxies.append(proxy_b)
        # Bind without listening and retain the socket: no other process can claim the refused endpoint during the case.
        refused = stack.enter_context(socket.socket(socket.AF_INET, socket.SOCK_STREAM))
        refused.bind(('127.0.0.1', 0))
        first = '127.0.0.1:%d' % refused.getsockname()[1] if mode in ('fallback', 'precreated') else proxy_a.address
        next_proxy = proxy_a.address if mode in ('fallback', 'precreated') else proxy_b.address
        extra_port = None
        if form == 'bare-port':
            next_proxy, extra_port = '::1', proxy_a.server.server_address[1]
        elif form == 'bracket-port':
            next_proxy, extra_port = '[::1]', proxy_a.server.server_address[1]
        ready = folder / 'gate-ready'
        release = folder / 'gate-release'
        extra = ['-VaRestTestServer=' + plain, '-VaRestTestServerTls=' + tls,
                 '-VaRestRouteCase=' + ident, '-VaRestRouteMode=' + mode,
                 '-VaRestRouteFirst=' + first, '-VaRestRouteNext=' + next_proxy,
                 '-VaRestRouteTrace=' + str(folder / 'trace.json'),
                 '-VaRestRouteCa=' + str(ROOT / 'Tools/testserver/certs/test-ca.pem'),
                 '-VaRestRoutePath=' + path, '-VaRestRouteGateReady=' + str(ready),
                 '-VaRestRouteGateRelease=' + str(release)]
        if extra_port is not None:
            extra.append('-VaRestRouteProxyPort=' + str(extra_port))
        if use_tls:
            extra.append('-VaRestRouteTls')
        if provided:
            extra.append('-VaRestRouteProvided')
        result.update(args=extra, first_proxy=first, next_proxy=next_proxy, source_key=source_key,
                      target_key=target_key, target_port=ports[1 if use_tls else 0],
                      proxy_a_address=proxy_a.address, proxy_b_address=proxy_b.address,
                      peer_a=dict(host=proxy_a.server.server_address[0], port=proxy_a.server.server_address[1]),
                      peer_b=dict(host=proxy_b.server.server_address[0], port=proxy_b.server.server_address[1]))
        save(folder / 'acquisition.json', result)
        environment = child_environment(os.environ, extra)
        result['child_argv'] = ['bash', str(ROOT / 'Tools/run_tests.sh'), '--no-build', 'Probe.VaRest.Lws.Route.Attempt']
        result['child_cwd'] = str(ROOT)
        with (folder / 'runner.log').open('wb') as stream:
            process = subprocess.Popen(result['child_argv'], cwd=ROOT, env=environment,
                                       stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
            observe_child(process, folder, proxy_a, held, ready, release, result, environment)
            save(folder / 'proxy-before-exit.json', proxy_a.snapshot())
            save(folder / 'proxy-b-before-exit.json', proxy_b.snapshot())
            save(folder / 'result.partial.json', result)
        # A failed drain remains false, is persisted below and aborts the matrix before another launch.
        snapshot_stimulus(folder, result, proxies)
    # A partial acquisition still has typed files, raw errors and a stable manifest.
    for name in ('runner.log', 'editor.log'):
        if not (folder / name).exists():
            (folder / name).write_text('No child log: acquisition failed before launch.\n')
    for name in ('proxy-a.json', 'proxy-b.json', 'acquisition.json'):
        if not (folder / name).exists():
            save(folder / name, dict(acquisition_error='acquisition incomplete'))
    for name, key in (('source', source_key), ('target', target_key), ('neighbor', ident + '-neighbor')):
        try:
            value = direct_json(plain, '/ws/journal?key=' + key + '&wait_ms=100', raw_path=folder / (name + '.raw'))
        except (OSError, ValueError, RuntimeError) as error:
            value = dict(acquisition_error=str(error))
            result['errors'].append(name + ': ' + str(error))
        save(folder / (name + '.json'), value)
    return result

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=pathlib.Path)
    parser.add_argument('--only', choices=[c[0] for c in CASES])
    parser.add_argument('--selftest', action='store_true')
    args = parser.parse_args()
    if args.selftest:
        return int(not unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ControllerTest)).wasSuccessful())
    if args.out is None:
        parser.error('--out required outside selftest')
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    if any(c.isspace() for c in str(args.out)):
        parser.error('output path must be whitespace-free for runner arguments')
    items = [item for item in CASES if not args.only or item[0] == args.only]
    try:
        with target_server(args.out) as (plain, tls):
            results = acquire_matrix(args.out, items, lambda folder, item: one(folder, item, plain, tls))
    except AcquisitionAborted as error:
        print(str(error), file=sys.stderr)
        return 1
    return int(any(r['runner_rc'] != 0 or r['errors'] for r in results if not r.get('unsupported_ipv6')))
if __name__ == '__main__':
    raise SystemExit(main())
