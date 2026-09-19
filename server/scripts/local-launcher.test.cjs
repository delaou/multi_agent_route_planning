const { test } = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http');
const { createGateway, recoverBackend, getJson } = require('./local-launcher.cjs');
const listen = server => new Promise(resolve => server.listen(0, '127.0.0.1', resolve));

test('gateway streams POST payloads, preserves errors, and reports unavailable backend', async t => {
  const backend = http.createServer((req, res) => {
    req.setEncoding('utf8');
    let body = ''; req.on('data', chunk => body += chunk);
    req.on('end', () => {
      res.writeHead(req.url === '/reject' ? 409 : 200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ ok: true, body, method: req.method }));
    });
  });
  await listen(backend);
  const state = { ready: false, upstream: '127.0.0.1' };
  const gateway = createGateway(state, backend.address().port);
  await listen(gateway);
  t.after(() => { gateway.shutdown(); backend.close(); });
  const url = `http://127.0.0.1:${gateway.address().port}`;
  assert.equal((await fetch(url)).status, 503);
  state.ready = true;
  const payload = JSON.stringify({ text: '列车状态'.repeat(10000) });
  const echoed = await (await fetch(url, { method: 'POST', body: payload })).json();
  assert.ok(echoed.body === payload, 'UTF-8 payload must survive streaming unchanged');
  assert.equal(echoed.method, 'POST');
  assert.equal((await fetch(url + '/reject')).status, 409);
  assert.equal((await getJson(url + '/__launcher/health')).ready, true);
  await new Promise(resolve => backend.close(resolve));
  assert.equal((await fetch(url)).status, 502);
});

test('recovery refreshes WSL address without restarting a live process', async () => {
  let starts = 0;
  const state = {};
  await recoverBackend({ status: async () => 'RUNNING 123', start: async () => starts++,
    addresses: async () => ['10.0.0.1', '10.0.0.2'],
    probe: async ip => { if (ip.endsWith('.1')) throw Error('old IP'); return { ok: true }; }
  }, state);
  assert.equal(starts, 0); assert.equal(state.upstream, '10.0.0.2'); assert.equal(state.ready, true);
});

test('recovery starts an exited owned backend; does not kill a hung backend', async () => {
  let starts = 0;
  const adapter = { status: async () => 'STOPPED', start: async () => starts++,
    addresses: async () => ['127.0.0.1'], probe: async () => ({ ok: true }) };
  await recoverBackend(adapter, {}); assert.equal(starts, 1);
  adapter.status = async () => 'RUNNING 123'; adapter.probe = async () => { throw Error('hung'); };
  await assert.rejects(recoverBackend(adapter, {}), /not be forcibly restarted/);
  assert.equal(starts, 1);
});

test('health check has a bounded timeout', async t => {
  const sockets = new Set();
  const server = http.createServer(() => {});
  server.on('connection', socket => sockets.add(socket));
  await listen(server);
  t.after(() => { for (const socket of sockets) socket.destroy(); server.close(); });
  await assert.rejects(getJson(`http://127.0.0.1:${server.address().port}`, 100), /timeout/);
});

test('occupied frontend port is not replaced', async t => {
  const owner = http.createServer((req, res) => res.end('existing service'));
  await listen(owner);
  const candidate = createGateway({ ready: false }, 18080);
  t.after(() => { candidate.shutdown(); owner.close(); });
  await assert.rejects(new Promise((resolve, reject) => {
    candidate.once('error', reject);
    candidate.listen(owner.address().port, '127.0.0.1', resolve);
  }), { code: 'EADDRINUSE' });
  assert.equal(await (await fetch(`http://127.0.0.1:${owner.address().port}`)).text(), 'existing service');
});
