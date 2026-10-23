#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
"""B1 raw proxy matrix. No build, no production classifier, no HTTP control in UE."""
import argparse
import contextlib
import hashlib
import http.client
import http.server
import json
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import threading
import time
import unittest
import urllib.parse
import uuid

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'Tools' / 'testserver'))
sys.dont_write_bytecode = True
from proxy import Proxy

PROXY_VARS = ('http_proxy', 'https_proxy', 'HTTP_PROXY', 'HTTPS_PROXY',
              'all_proxy', 'ALL_PROXY', 'no_proxy', 'NO_PROXY')
CASES = tuple((mode + '-' + scheme, mode, scheme == 'wss')
              for mode in ('direct', 'explicit', 'deny', 'close', 'silent',
                           'redirect', 'env', 'precedence', 'env-noproxy')
              for scheme in ('ws', 'wss')) + (
                  ('redirect-cross-ws', 'redirect-cross', False),
                  ('untrusted-wss', 'untrusted', True),
                  ('upgrade403-ws', 'upgrade403', False),
                  ('upgrade403-wss', 'upgrade403', True))


def case_definition(case_id):
    # Expected semantics come only from the code's fixed CASES tuple, never collected labels.
    for known_id, mode, use_tls in CASES:
        if case_id == known_id:
            return dict(mode=mode, tls=use_tls, untrusted=mode == 'untrusted',
                        redirect_hostname_skip=mode.startswith('redirect'),
                        primary_budget_seconds=30.0 if mode == 'silent' else 8.0,
                        explicit_proxy=mode not in ('direct', 'env', 'env-noproxy'),
                        fixture_mode=mode if mode in ('deny', 'close', 'silent') else 'tunnel',
                        fixture_lifetime_seconds=40.0 if mode == 'silent' else 12.0)
    raise ValueError('unknown immutable case: ' + str(case_id))


def write_json(path, value):
    pathlib.Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def direct_json(base, path):
    parsed = urllib.parse.urlsplit(base)
    if parsed.scheme != 'http' or parsed.hostname != '127.0.0.1' or not parsed.port:
        raise ValueError('control must be numeric loopback HTTP')
    conn = http.client.HTTPConnection('127.0.0.1', parsed.port, timeout=5)
    try:
        conn.request('GET', path)
        response = conn.getresponse()
        body = response.read(2 * 1024 * 1024 + 1)
        if response.status != 200 or len(body) > 2 * 1024 * 1024:
            raise RuntimeError('invalid control response')
        return json.loads(body)
    finally:
        conn.close()


def child_environment(base, extra, proxy='', no_proxy=''):
    result = {k: v for k, v in base.items() if k not in PROXY_VARS
              and not k.startswith('VAREST_LWS_PROXY_')}
    if any(c.isspace() for arg in extra for c in arg):
        raise ValueError('runner extra arguments require whitespace-free paths')
    result.update(VAREST_NO_TESTSERVER='1', VAREST_TEST_TIMEOUT='90',
                  VAREST_EXTRA_ARGS=' '.join(extra))
    if proxy:
        result['VAREST_LWS_PROXY_ENV'] = proxy
    if no_proxy:
        result['VAREST_LWS_PROXY_NO_PROXY'] = no_proxy
    return result


def check_trace(trace, case_id):
    problems = []
    if trace.get('schema') != 1 or trace.get('case') != case_id:
        problems.append('trace identity/schema')
    events = trace.get('events', [])
    if not events or [e.get('seq') for e in events] != list(range(1, len(events) + 1)):
        problems.append('missing/unordered events')
    if trace.get('dropped_events') != 0:
        problems.append('dropped trace events')
    if not trace.get('shutdown_completed'):
        problems.append('context shutdown incomplete')
    if not trace.get('destroyed'):
        problems.append('attempt destruction not recorded')
    if not trace.get('primary_completed'):
        problems.append('attempt observation deadline')
    for event in events:
        if event.get('reason') == 1:
            if isinstance(event.get('len'), int) and event['len'] < 0:
                problems.append('negative callback length')
            if not isinstance(event.get('input_present'), bool) or not isinstance(event.get('len'), int):
                problems.append('error input metadata missing')
            try:
                raw = bytes.fromhex(event['hex'])
                expected = min(event['len'], 256) if event['input_present'] else 0
                if len(raw) != expected:
                    problems.append('error byte capture differs from length/presence')
            except (KeyError, TypeError, ValueError):
                problems.append('malformed error hex')
    return problems


def group_running(group_id):
    # Linux-only driver: a dead runner can leave its editor alive in the owned group.
    for entry in pathlib.Path('/proc').iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            fields = (entry / 'stat').read_text().rsplit(')', 1)[1].split()
            if int(fields[2]) == group_id and fields[0] not in ('Z', 'X'):
                return True
        except (OSError, IndexError, ValueError):
            continue
    return False


def stop_group(process):
    # Never skip descendants merely because the shell leader has exited.
    for sig, seconds in ((signal.SIGTERM, 3), (signal.SIGKILL, 2)):
        try:
            os.killpg(process.pid, sig)
        except ProcessLookupError:
            break
        deadline = time.monotonic() + seconds
        while group_running(process.pid) and time.monotonic() < deadline:
            threading.Event().wait(.02)
        if not group_running(process.pid):
            break
    if process.poll() is None:
        process.wait(timeout=2)
    if group_running(process.pid):
        raise RuntimeError('owned editor process group did not drain')


def invoke_runner(env, output):
    with output.open('wb') as stream:
        process = subprocess.Popen(['bash', str(ROOT / 'Tools' / 'run_tests.sh'), '--no-build',
                                    'Probe.VaRest.Lws.Proxy.Attempt'], cwd=ROOT, env=env,
                                   stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            return process.wait(timeout=120)
        except subprocess.TimeoutExpired:
            return 124
        finally:
            stop_group(process)


@contextlib.contextmanager
def target_server(directory):
    ready = directory / 'server.ready'
    with (directory / 'server.log').open('wb') as log:
        process = subprocess.Popen([sys.executable, '-I', str(ROOT / 'Tools' / 'testserver' / 'server.py'),
                                    '--host', '127.0.0.1', '--port', '0', '--tls-port', '0',
                                    '--ready-file', str(ready), '--parent-pid', str(os.getpid())],
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 10
            while not ready.exists() and process.poll() is None and time.monotonic() < deadline:
                threading.Event().wait(.05)
            if not ready.exists():
                raise RuntimeError('target fixture failed; see server.log')
            urls = ready.read_text().splitlines()
            plain = next(u for u in urls if u.startswith('http://'))
            tls = next(u for u in urls if u.startswith('https://'))
            yield plain, tls
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


def run_case(directory, case, plain, tls):
    case_id, mode, use_tls = case
    definition = case_definition(case_id)
    directory.mkdir()
    source_key = 'b1-' + uuid.uuid4().hex
    target_key = 'b1-' + uuid.uuid4().hex
    ports = [urllib.parse.urlsplit(plain).port, urllib.parse.urlsplit(tls).port]
    source_path = '/ws/echo?key=' + source_key
    if mode.startswith('redirect'):
        destination = ('wss' if use_tls or mode == 'redirect-cross' else 'ws')
        destination_port = ports[1] if destination == 'wss' else ports[0]
        location = '%s://localhost:%d/ws/echo?key=%s' % (destination, destination_port, target_key)
        # Redirects to localhost can resolve ::1 first on this host. Use numeric TCP authority.
        # Raw TLS redirect hostname checks then legitimately measure the pinned archive's IP behavior.
        # Skip hostname check only on cross/redirect controls; trusted initial wss remains a separate row.
        location = location.replace('://localhost:', '://127.0.0.1:')
        source_path = '/ws/status/302?key=' + source_key + '&location=' + urllib.parse.quote(location, safe='')
    elif mode == 'upgrade403':
        source_path = '/ws/status/403?key=' + source_key
    errors = []
    result = dict(case=case_id, mode=mode, tls=use_tls, untrusted=definition['untrusted'],
                  redirect_hostname_skip=definition['redirect_hostname_skip'], source_key=source_key,
                  target_key=target_key, target_port=ports[1] if use_tls else ports[0], control_access='parent-http.client-direct', harness_errors=errors)
    fixture_mode = mode if mode in ('deny', 'close', 'silent') else 'tunnel'
    with Proxy(fixture_mode, ports) as proxy_a, Proxy('tunnel', ports) as proxy_b:
        extra = ['-VaRestTestServer=' + plain, '-VaRestTestServerTls=' + tls,
                 '-VaRestProxyCase=' + case_id, '-VaRestProxyMode=' + mode, '-VaRestProxyTrace=' + str(directory / 'trace.json'),
                 '-VaRestProxyCa=' + str(ROOT / 'Tools/testserver/certs/test-ca.pem'),
                 '-VaRestProxyPath=' + source_path]
        if use_tls:
            extra.append('-VaRestProxyTls')
        if mode == 'untrusted':
            extra.append('-VaRestProxyUntrusted')
        if mode.startswith('redirect'):
            extra.append('-VaRestProxyRedirectControl')
        if mode not in ('direct', 'env', 'env-noproxy'):
            extra.append('-VaRestTestProxy=' + proxy_a.address)
        environment_proxy = proxy_b.address if mode in ('env', 'precedence', 'env-noproxy') else ''
        env = child_environment(os.environ, extra, environment_proxy, '*' if mode == 'env-noproxy' else '')
        result.update(proxy_a=proxy_a.address, proxy_b=proxy_b.address, env_proxy=environment_proxy,
                      no_proxy='*' if mode == 'env-noproxy' else '', args=extra)
        try:
            result['runner_rc'] = invoke_runner(env, directory / 'runner.txt')
            result['group_drained'] = True
            if result['runner_rc'] != 0:
                errors.append('editor/automation runner failed')
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
            result['runner_rc'] = -1
            result['group_drained'] = False
            errors.append('runner setup/cleanup: ' + str(error))
        finally:
            host = pathlib.Path(env.get('VAREST_HOST', '/w/Unreal/VaRestHost'))
            log = host / 'Saved/Logs/VaRestTests.log'
            if log.exists():
                shutil.copyfile(log, directory / 'editor.log')
            else:
                errors.append('editor log missing')
        if not proxy_a.wait_idle(2) or not proxy_b.wait_idle(2):
            errors.append('proxy still active after child exit')
    # Both fixtures closed: no future target request can originate from this attempt.
    write_json(directory / 'proxy-a.json', proxy_a.snapshot())
    write_json(directory / 'proxy-b.json', proxy_b.snapshot())
    for label, key in (('source', source_key), ('target', target_key)):
        try:
            journal = direct_json(plain, '/ws/journal?key=' + key + '&wait_ms=2000')
            write_json(directory / (label + '.json'), journal)
            if journal.get('dropped') != 0:
                errors.append(label + ' journal dropped events')
            if journal.get('connections', 0) and not journal.get('ended'):
                errors.append(label + ' journal not ended')
        except (OSError, ValueError, RuntimeError, http.client.HTTPException) as error:
            errors.append(label + ' control fetch: ' + str(error))
    try:
        trace = json.loads((directory / 'trace.json').read_text())
        errors.extend(check_trace(trace, case_id))
        if mode in ('env', 'precedence', 'env-noproxy') and not trace.get('configured_proxy_absent'):
            errors.append('engine configured proxy not absent; null-fallback claim invalid')
    except (OSError, ValueError) as error:
        errors.append('trace missing/malformed: ' + str(error))
    for proxy in (proxy_a, proxy_b):
        if proxy.snapshot()['dropped']:
            errors.append('proxy journal dropped events')
    write_json(directory / 'case.json', result)
    return result


def run_matrix(out, only):
    out = out.resolve()
    out.mkdir(parents=True, exist_ok=False)  # never overwrite a prior evidence run
    selected = [case for case in CASES if only is None or case[0] == only]
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    dirty = subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True)
    manifest = dict(schema=1, commit=commit, dirty=dirty, cases=[c[0] for c in selected],
                    scope='Linux modular editor raw rig; no native/cooked runtime claim',
                    acquisition_only=True, results=[])
    write_json(out / 'manifest.json', manifest)
    with target_server(out) as (plain, tls):
        for case in selected:
            record = run_case(out / case[0], case, plain, tls)
            manifest['results'].append(dict(case=case[0], harness_ok=not record['harness_errors']))
            write_json(out / 'manifest.json', manifest)
    hashes = {str(path.relative_to(out)): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in out.rglob('*') if path.is_file()}
    write_json(out / 'sha256.json', hashes)
    return 0 if all(row['harness_ok'] for row in manifest['results']) else 1


class ControllerTests(unittest.TestCase):
    def test_child_environment_is_explicit(self):
        base = dict(http_proxy='secret', HTTPS_PROXY='secret', no_proxy='*',
                    VAREST_LWS_PROXY_ENV='old', VAREST_EXTRA_ARGS='old', PATH='/bin')
        clean = child_environment(base, ['-VaRestProxyCase=x'])
        self.assertFalse(set(PROXY_VARS).intersection(clean))
        self.assertNotIn('VAREST_LWS_PROXY_ENV', clean)
        picked = child_environment(base, ['-VaRestProxyCase=x'], '127.0.0.1:1234', '*')
        self.assertEqual(picked['VAREST_LWS_PROXY_ENV'], '127.0.0.1:1234')
        self.assertEqual(picked['VAREST_LWS_PROXY_NO_PROXY'], '*')
        self.assertEqual(base['http_proxy'], 'secret')
        with self.assertRaises(ValueError):
            child_environment(base, ['-VaRestProxyTrace=/bad path'])

    def test_direct_control_under_poisoned_proxy(self):
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                body = b'{"control":"direct"}'
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            def log_message(self, *_):
                pass
        with http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
            thread = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': .05})
            thread.start()
            try:
                env = dict(os.environ)
                env.update({key: 'http://127.0.0.1:1' for key in PROXY_VARS})
                url = 'http://127.0.0.1:%d' % server.server_port
                result = subprocess.run([sys.executable, '-I', __file__, '--control-url', url],
                                        env=env, capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout), {'control': 'direct'})
            finally:
                server.shutdown()
                thread.join(1)

    def test_harness_failure_is_not_negative_outcome(self):
        trace = dict(schema=1, case='deny-ws', events=[dict(seq=1, reason=1,
                     input_present=False, len=0, hex='')], dropped_events=0,
                     primary_completed=True, shutdown_completed=True, destroyed=True, opened=False)
        self.assertEqual(check_trace(trace, 'deny-ws'), [])
        trace['events'][0]['input_present'] = True
        trace['events'][0]['len'] = 4
        self.assertIn('error byte capture differs from length/presence', check_trace(trace, 'deny-ws'))
        trace['dropped_events'] = 1
        self.assertIn('dropped trace events', check_trace(trace, 'deny-ws'))
        trace['case'] = 'other'
        self.assertIn('trace identity/schema', check_trace(trace, 'deny-ws'))

    def test_cleanup_after_runner_leader_exits(self):
        child_code = ('import subprocess,sys; '
                      'p=subprocess.Popen([sys.executable,"-c","import time; time.sleep(60)"],'
                      'stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL); '
                      'print(p.pid,flush=True)')
        process = subprocess.Popen([sys.executable, '-c', child_code], stdout=subprocess.PIPE,
                                   text=True, start_new_session=True)
        try:
            orphan_pid = int(process.stdout.readline())
            self.assertGreater(orphan_pid, 0)
            self.assertEqual(process.wait(timeout=3), 0)
            self.assertTrue(group_running(process.pid))
            stop_group(process)
            self.assertFalse(group_running(process.pid))
        finally:
            stop_group(process)
            process.stdout.close()

    def test_matrix_unique_and_required_controls(self):
        ids = [c[0] for c in CASES]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertTrue({'direct-ws', 'direct-wss', 'explicit-ws', 'explicit-wss',
                         'deny-ws', 'deny-wss', 'close-ws', 'close-wss', 'silent-ws', 'silent-wss',
                         'redirect-cross-ws', 'untrusted-wss', 'upgrade403-wss', 'env-ws',
                         'precedence-wss', 'env-noproxy-wss'}.issubset(ids))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=pathlib.Path)
    parser.add_argument('--only', choices=[c[0] for c in CASES])
    parser.add_argument('--selftest', action='store_true')
    parser.add_argument('--control-url')
    args = parser.parse_args()
    if args.selftest:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(ControllerTests)
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    if args.control_url:
        print(json.dumps(direct_json(args.control_url, '/')))
        return 0
    if args.out is None:
        parser.error('--out is required for a matrix')
    return run_matrix(args.out, args.only)


if __name__ == '__main__':
    sys.exit(main())
