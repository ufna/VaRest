// Local official Socket.IO reference peer. No protocol implementation or mock.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { createRequire } from 'node:module';
import { pathToFileURL, fileURLToPath } from 'node:url';

const options = new Map();
for (let i = 2; i < process.argv.length; i += 2) {
  const key = process.argv[i], value = process.argv[i + 1];
  if (!['--server', '--port', '--receipts', '--modules-root', '--demo-int73'].includes(key) || value === undefined || options.has(key))
    throw new Error('invalid reference peer option');
  options.set(key, value);
}
const aliases = { '4.8': ['socket.io', '4.8.4'], '4.5': ['socket.io-v45', '4.5.4'], '4.4': ['socket.io-v44', '4.4.1'], '2': ['socket.io-v2', '2.5.1'] };
const selected = options.get('--server');
if (!Object.hasOwn(aliases, selected)) throw new Error('explicit fixed server version required');
// A separate public Blueprint demo contract; ordinary reference cases keep the exact string oracle.
if (options.has('--demo-int73') && (options.get('--demo-int73') !== '1' || selected !== '4.8'))
  throw new Error('int73 demo requires explicit 1 and official 4.8 server');
const demoInt73 = options.has('--demo-int73');
const portText = options.get('--port');
if (!/^[1-9][0-9]{3,4}$/.test(portText ?? '')) throw new Error('explicit unprivileged port required');
const basePort = Number(portText);
if (basePort < 1024 || basePort > 65532) throw new Error('reference port range');
if (!process.execArgv.includes('--no-global-search-paths')) throw new Error('disable global module lookup for reference qualification');
const root = options.has('--modules-root') ? path.resolve(options.get('--modules-root')) : path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(pathToFileURL(path.join(root, 'package.json')));
const [alias, exactVersion] = aliases[selected];
// Read the actual selected package identity; Node resolves no user/global packages.
const selectedPackage = JSON.parse(fs.readFileSync(path.join(root, 'node_modules', alias, 'package.json'), 'utf8'));
if (selectedPackage.name !== 'socket.io' || selectedPackage.version !== exactVersion) throw new Error('fixed reference package mismatch');
const library = require(alias);
const receiptPath = options.get('--receipts');
if (!receiptPath) throw new Error('exclusive owned receipt path required');
const fd = fs.openSync(receiptPath, 'wx', 0o600);
let sequence = 0, errors = 0, stopping = false;
const timers = new Set(), servers = [], engines = new WeakMap();
function record(event, fields = {}) {
  fs.writeSync(fd, JSON.stringify({ seq: ++sequence, event, server: exactVersion, ...fields }) + '\n');
}
function later(fn, ms) {
  const timer = setTimeout(() => { timers.delete(timer); fn(); }, ms);
  timers.add(timer); return timer;
}
const digest = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const bytes = Buffer.from([0, 255, 0, 128, 1]);
function check(condition, category) {
  if (!condition) { ++errors; record('error', { category }); }
  return condition;
}
function safeReason(reason) {
  return ['transport close', 'transport error', 'forced close', 'ping timeout', 'server shutting down', 'forced server close', 'client namespace disconnect', 'server namespace disconnect'].includes(reason) ? reason : 'other';
}
function response(socket, name, value) { socket.emit(name, value); }
function binaryEqual(value, expected = bytes) { return Buffer.isBuffer(value) && value.equals(expected); }

function installNamespace(io, name, profile) {
  const namespace = io.of(name);
  if (name === '/slow') namespace.use((_socket, _next) => { record('namespace.pending', { profile, namespace: name }); });
  if (name === '/auth') namespace.use((socket, next) => {
    const auth = socket.handshake.auth ?? {};
    const mode = socket.handshake.query.mode;
    // Only fixed public demo values are accepted; arbitrary credentials are never logged.
    const tokenOK = auth.access === 'Bearer varest-public-sio-2';
    if (mode === 'always-refuse' || !tokenOK) {
      record('auth.refused', { profile, namespace: name, code: 401 });
      const error = new Error('unauthorized'); error.data = { code: 401 }; next(error);
    } else { record('auth.accepted', { profile, namespace: name }); next(); }
  });
  namespace.on('connection', socket => {
    const engine = engines.get(socket.conn);
    if (!check(Boolean(engine), 'namespace_without_engine')) return;
    record('namespace.connected', { profile, namespace: name, connection: engine.ordinal, recovered: socket.recovered === true });
    socket.on('disconnect', reason => record('namespace.disconnected', { profile, namespace: name, connection: engine.ordinal, reason: safeReason(reason) }));
    socket.on('error', () => { ++errors; record('error', { category: 'namespace_error', profile, namespace: name }); });
    socket.on('echo', (value, ack) => {
      if (!check(demoInt73 ? value === 73 : value === 'varest-public-sio', 'unexpected_echo_value')) return;
      record('echo.received', { profile, namespace: name, connection: engine.ordinal,
        ...(demoInt73 ? { demo: 'public-int73', validated_number: 73 } : {}) });
      if (typeof ack === 'function') ack(value);
      response(socket, 'echo', value);
    });
    socket.on('bytes', (...args) => {
      const ack = typeof args.at(-1) === 'function' ? args.pop() : null;
      const valid = [1, 9, 10].includes(args.length) && args.every(value => binaryEqual(value));
      if (!check(valid, 'binary_argument_mismatch')) return;
      record('binary.received', { profile, namespace: name, count: args.length, bytes_each: bytes.length, sha256_each: digest(bytes) });
      if (ack) ack(...args);
      socket.emit('binary', ...args);
    });
    socket.on('nested', (value, ack) => {
      // This check concerns nested byte ownership, not casing/naming-rule qualification.
      const outer = value && typeof value === 'object' ? Object.values(value) : [];
      const inner = outer.length === 1 && outer[0] && typeof outer[0] === 'object' ? Object.values(outer[0]) : [];
      if (!check(inner.length === 1 && binaryEqual(inner[0]), 'nested_binary_mismatch')) return;
      record('binary.nested', { profile, namespace: name, bytes: bytes.length, sha256: digest(bytes) });
      if (typeof ack === 'function') ack(value);
    });
    socket.on('sized', (value, ack) => {
      if (!check(typeof value === 'string' && Buffer.byteLength(value) === 899986 && /^x+$/.test(value) && engine.lastTextMessageBytes === 900000, 'sized_payload_mismatch')) return;
      record('payload.received', { profile, namespace: name, payload_bytes: Buffer.byteLength(value), wire_bytes: engine.lastTextMessageBytes, sha256: digest(value) });
      const receipt = { payload_bytes: Buffer.byteLength(value), wire_bytes: engine.lastTextMessageBytes };
      if (typeof ack === 'function') ack(receipt);
      socket.emit('report-sized', receipt);
    });
    socket.on('upload', (...args) => {
      const ack = typeof args.at(-1) === 'function' ? args.pop() : null;
      const good = args.length === 8 && args.every(v => Buffer.isBuffer(v) && v.length === 1024 * 1024 && v.every(b => b === 0x5a));
      if (!check(engine.throttleActive && typeof engine.uploadStarted === 'bigint', 'upload_without_real_throttle_control')) return;
      const pongs = engine.pongs - (engine.uploadStartPongs ?? engine.pongs);
      const pauses = engine.pauses - (engine.uploadStartPauses ?? engine.pauses);
      engine.throttleActive = false; socket.conn.request.socket.resume();
      if (!check(good, 'upload_bytes_mismatch')) return;
      const elapsedMs = Number(process.hrtime.bigint() - engine.uploadStarted) / 1e6;
      record('upload.received', { profile, namespace: name, bytes: 8 * 1024 * 1024, message_parts: args.length, pongs_during: pongs, pause_events: pauses, elapsed_ms: elapsedMs });
      if (ack) ack({ bytes: 8 * 1024 * 1024, pongs, pauses, elapsed_ms: elapsedMs });
    });
    socket.on('__test', (command, ack) => {
      const answer = value => { if (typeof ack === 'function') ack(value); };
      if (!command || typeof command !== 'object' || typeof command.op !== 'string') { check(false, 'invalid_control_shape'); return; }
      switch (command.op) {
        case 'report': answer({ connection: engine.ordinal, pongs: engine.pongs, pauses: engine.pauses, recovered: socket.recovered === true }); break;
        case 'drop': {
          const room = socket.id; // kept only in memory, never written to the receipt.
          namespace.to(room).emit('counter', 1);
          namespace.to(room).emit('counter-binary', 2, bytes);
          record('recovery.before_drop', { profile, namespace: name, connection: engine.ordinal });
          answer({ scheduled: true });
          later(() => socket.conn.close(), 30);
          later(() => {
            if (!check(!socket.connected, 'outage_event_before_real_disconnect')) return;
            namespace.to(room).emit('counter', 3);
            namespace.to(room).emit('counter', 4);
            record('recovery.offline_events', { profile, namespace: name, connection: engine.ordinal, first: 3, last: 4 });
          }, 80);
          break;
        }
        case 'server-acks': {
          answer({ scheduled: true });
          if (!check(selected === '4.8', 'unsupported_ack_profile')) return;
          socket.timeout(1500).emitWithAck('server-plain', 7).then(value => {
            const good = value === 'typed-ok'; check(good, 'server_plain_ack_mismatch');
            record('ack.plain', { profile, namespace: name, once: good }); response(socket, 'report-plain', good);
          }, () => { check(false, 'server_plain_ack_missing'); response(socket, 'report-plain', false); });
          socket.timeout(1500).emitWithAck('server-binary', bytes).then(value => {
            const good = binaryEqual(value); check(good, 'server_binary_ack_mismatch');
            record('ack.binary', { profile, namespace: name, once: good }); response(socket, 'report-binary', good);
          }, () => { check(false, 'server_binary_ack_missing'); response(socket, 'report-binary', false); });
          socket.timeout(250).emit('server-no-reply', 9, err => {
            const good = Boolean(err); check(good, 'server_timeout_missing');
            record('ack.timeout', { profile, namespace: name, timed_out: good }); response(socket, 'report-timeout', good);
          });
          break;
        }
        case 'stale-ack': {
          answer({ scheduled: true });
          if (!check(selected === '4.8' && name === '/admin', 'stale_ack_wrong_namespace')) return;
          socket.timeout(400).emit('server-late', 11, err => {
            const good = Boolean(err); check(good, 'stale_reply_reached_server');
            record('ack.stale_not_received', { profile, namespace: name, not_acknowledged: good });
            io.of('/').emit('report-stale', good);
          });
          later(() => socket.disconnect(false), 30);
          break;
        }
        case 'disconnect-admin': {
          answer({ scheduled: true });
          later(() => {
            const sockets = io.of('/admin').sockets;
            for (const s of sockets instanceof Map ? sockets.values() : Object.values(sockets)) s.disconnect(false);
          }, 20);
          break;
        }
        case 'large-inbound': {
          const body = Buffer.alloc(2 * 1024 * 1024, 0x3c); answer({ bytes: body.length });
          socket.emit('large-inbound', body); record('payload.sent', { profile, namespace: name, bytes: body.length, sha256: digest(body) }); break;
        }
        case 'stall-window': {
          const before = engine.pongs; answer({ scheduled: true });
          later(() => { const count = engine.pongs - before; record('heartbeat.stall_window', { profile, namespace: name, pongs: count }); response(socket, 'report-stall', count); }, 1400);
          break;
        }
        case 'throttle-upload': {
          if (!check(profile === 'heartbeat' && !engine.throttleActive, 'invalid_throttle_profile')) return;
          engine.uploadStartPongs = engine.pongs; engine.uploadStartPauses = engine.pauses;
          engine.uploadStarted = null; engine.throttleActive = true;
          // Each actual one-MiB attachment boundary pauses the real TCP reader.
          // Eight messages form one logical upload; PONG data can interleave at
          // message boundaries, never inside a fragmented WebSocket message.
          answer({ ready: true }); break;
        }
        default: check(false, 'unknown_control_operation'); break;
      }
    });
  });
}

let ordinal = 0;
for (const [index, profile] of ['base', 'payload', 'heartbeat'].entries()) {
  const server = http.createServer((request, response) => {
    if (request.method !== 'POST' || request.url !== '/token') { response.writeHead(404); response.end('not a reference Engine.IO route'); return; }
    let body = '', oversized = false;
    request.setEncoding('utf8');
    request.on('data', chunk => { if (body.length + chunk.length > 1024) oversized = true; else body += chunk; });
    request.on('end', () => {
      let value = null; try { if (!oversized) value = JSON.parse(body); } catch {}
      if (!value || value.refreshToken !== 'varest-public-sio-refresh') { response.writeHead(400); response.end(); return; }
      record('auth.refresh', { profile, method: 'POST', accepted: true });
      response.writeHead(200, { 'Content-Type': 'application/json' });
      response.end(JSON.stringify({ accessToken: 'varest-public-sio-2', refreshToken: 'varest-public-sio-refresh', expiresIn: 3600 }));
    });
  });
  const config = { serveClient: false, transports: ['websocket'], path: '/socket.io/', perMessageDeflate: false,
    maxHttpBufferSize: profile === 'base' ? 1000000 : 100000000,
    pingInterval: profile === 'heartbeat' ? 300 : 25000, pingTimeout: profile === 'heartbeat' ? 200 : 20000 };
  if (selected === '4.8') config.connectionStateRecovery = { maxDisconnectionDuration: 120000, skipMiddlewares: false };
  const io = selected === '2' ? library(server, config) : new library.Server(server, config);
  io.engine.on('connection', connection => {
    const row = { ordinal: ++ordinal, pongs: 0, pauses: 0, throttleActive: false }; engines.set(connection, row);
    record('engine.connected', { profile, connection: row.ordinal });
    connection.on('packet', packet => {
      if (packet.type === 'pong') ++row.pongs;
      if (packet.type === 'message' && typeof packet.data === 'string') row.lastTextMessageBytes = Buffer.byteLength(packet.data) + 1;
      if (row.throttleActive && packet.type === 'message' && Buffer.isBuffer(packet.data) && packet.data.length === 1024 * 1024) {
        if (row.uploadStarted === null) row.uploadStarted = process.hrtime.bigint();
        const netSocket = connection.request.socket;
        netSocket.pause(); ++row.pauses;
        later(() => { if (!netSocket.destroyed) netSocket.resume(); }, 100);
      }
    });
    connection.on('close', reason => {
      row.throttleActive = false;
      record('engine.closed', { profile, connection: row.ordinal, reason: safeReason(reason), pongs: row.pongs });
    });
    connection.on('error', () => { ++errors; record('error', { category: 'engine_error', profile }); });
  });
  for (const name of ['/', '/room', '/admin', '/slow', '/auth']) installNamespace(io, name, profile);
  server.on('error', () => { ++errors; record('error', { category: 'listen_error', profile }); void stop(); });
  servers.push({ io, server, profile, port: basePort + index });
}

async function stop() {
  if (stopping) return;
  stopping = true;
  for (const timer of timers) clearTimeout(timer);
  const bound = setTimeout(() => { record('error', { category: 'stop_timeout' }); fs.closeSync(fd); process.exit(1); }, 5000);
  await Promise.all(servers.map(({ io }) => new Promise(resolve => {
    try { io.close(() => resolve()); } catch { ++errors; record('error', { category: 'close_exception' }); resolve(); }
  })));
  clearTimeout(bound); record('stopped', { errors }); fs.closeSync(fd); process.exit(errors ? 1 : 0);
}
process.once('SIGTERM', () => void stop());
process.once('SIGINT', () => void stop());
process.once('uncaughtException', () => { ++errors; record('error', { category: 'uncaught_exception' }); void stop(); });
process.once('unhandledRejection', () => { ++errors; record('error', { category: 'unhandled_rejection' }); void stop(); });
await Promise.all(servers.map(({ server, port }) => new Promise(resolve => server.listen(port, '127.0.0.1', resolve))));
record('ready', { profiles: servers.map(({ profile, port }) => ({ profile, port })), recovery_available: selected === '4.8',
  ...(demoInt73 ? { demo: 'public-int73' } : {}) });
console.log(`VAREST_SIO_REFERENCE_READY ${exactVersion} ${basePort} ${basePort + 1} ${basePort + 2}`);
later(() => { ++errors; record('error', { category: 'unclosed_reference_deadline' }); void stop(); }, 300000);
