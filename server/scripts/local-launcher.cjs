// Windows localhost gateway and WSL backend supervisor. No npm packages required.
const http = require('node:http');
const net = require('node:net');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { execFile } = require('node:child_process');
const { promisify } = require('node:util');
const run = promisify(execFile);
const healthPath = '/__launcher/health';

function getJson(url, timeout = 2000) {
  return new Promise((resolve, reject) => {
    const req = http.get(url, res => {
      res.setEncoding('utf8');
      let body = '';
      res.on('data', data => {
        body += data;
        if (body.length > 65536) req.destroy(new Error('Health response too large'));
      });
      res.on('error', reject);
      res.on('end', () => {
        if (res.statusCode !== 200) return reject(new Error(`HTTP ${res.statusCode}`));
        try { resolve(JSON.parse(body)); } catch (error) { reject(error); }
      });
    });
    const timer = setTimeout(() => req.destroy(new Error('Health check timeout')), timeout);
    req.on('close', () => clearTimeout(timer));
    req.on('error', reject);
  });
}

function createGateway(state, backendPort) {
  const sockets = new Set();
  const server = http.createServer((req, res) => {
    if (req.url === healthPath) {
      res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
      return res.end(JSON.stringify(state));
    }
    if (!state.ready || !net.isIPv4(state.upstream)) {
      res.writeHead(503, { 'Content-Type': 'text/plain; charset=utf-8', 'Retry-After': '5' });
      return res.end('服务暂不可用，启动工具正在检查连接。请稍后刷新。');
    }
    const upstream = http.request({ hostname: state.upstream, port: backendPort,
      method: req.method, path: req.url, headers: { ...req.headers,
        host: `${state.upstream}:${backendPort}`, connection: 'close' } }, response => {
      res.writeHead(response.statusCode, response.headers);
      response.pipe(res);
      response.on('error', () => res.destroy());
    });
    upstream.setTimeout(15000, () => upstream.destroy(new Error('Upstream timeout')));
    upstream.on('error', () => {
      if (!res.headersSent) res.writeHead(502, { 'Content-Type': 'text/plain; charset=utf-8' });
      res.end('后端连接中断，启动工具将自动检查恢复。');
    });
    res.on('close', () => { if (!res.writableFinished) upstream.destroy(); });
    req.on('aborted', () => upstream.destroy());
    req.pipe(upstream);
  });
  server.on('connection', socket => {
    sockets.add(socket); socket.on('close', () => sockets.delete(socket));
  });
  server.shutdown = () => { for (const socket of sockets) socket.destroy(); server.close(); };
  return server;
}

async function recoverBackend(adapter, state) {
  // Never kill an unresponsive live process: it may hold dispatch reservations.
  const status = await adapter.status();
  if (!status.startsWith('RUNNING ')) await adapter.start();
  const addresses = await adapter.addresses();
  for (const address of addresses) {
    try {
      if ((await adapter.probe(address)).ok === true) {
        state.upstream = address; state.ready = true; state.error = ''; return;
      }
    } catch (_) { /* Try the next address after a WSL network change. */ }
  }
  throw new Error('Backend has not responded. A live process will not be forcibly restarted.');
}

async function main() {
  const root = path.resolve(__dirname, '../..');
  const config = JSON.parse(fs.readFileSync(path.join(root, 'launcher.config.json'), 'utf8'));
  for (const key of ['listenPort', 'backendPort']) {
    if (!Number.isInteger(config[key]) || config[key] < 1025 || config[key] > 65535)
      throw new Error(`Invalid ${key}`);
  }
  if (config.listenPort === config.backendPort) throw new Error('Frontend and backend ports must differ');
  if (typeof config.distribution !== 'string' || !config.distribution) throw new Error('Set a WSL distribution');
  const runtimeDir = path.join(root, 'server/.local-launcher');
  fs.mkdirSync(runtimeDir, { recursive: true });
  const stopFile = path.join(runtimeDir, 'stop.json');
  const project = crypto.createHash('sha256').update(root.toLowerCase()).digest('hex');
  const address = `http://127.0.0.1:${config.listenPort}`;
  let previous;
  try { previous = await getJson(address + healthPath); } catch (_) {}
  if (process.argv.includes('--stop')) {
    if (!previous || previous.project !== project) throw new Error('No matching launcher is running');
    fs.writeFileSync(stopFile, JSON.stringify({ instance: previous.instance }));
    console.log('Stop requested. The launcher will stop its gateway and owned backend.'); return;
  }
  if (previous && previous.project === project) {
    console.log(`Already running: ${address} (ready=${previous.ready})`); return;
  }
  const state = { project, instance: crypto.randomUUID(), ready: false, upstream: '', error: '',
    checkedAt: '', pid: process.pid, backendStarts: 0 };
  const logFile = path.join(runtimeDir, 'launcher.log');
  function log(message) {
    if (fs.existsSync(logFile) && fs.statSync(logFile).size > 2 * 1024 * 1024)
      fs.renameSync(logFile, logFile + '.previous');
    const line = `${new Date().toISOString()} ${message}`;
    console.log(line); fs.appendFileSync(logFile, line + '\n');
  }
  const gateway = createGateway(state, config.backendPort);
  await new Promise((resolve, reject) => {
    gateway.once('error', reject); gateway.listen(config.listenPort, '127.0.0.1', resolve);
  });
  gateway.on('error', error => log(error.message));
  const wsl = async args => (await run('wsl.exe', ['-d', config.distribution, '--exec', ...args],
    { windowsHide: true, timeout: 20000, maxBuffer: 65536 })).stdout.trim();
  let backendScript;
  const script = async () => backendScript || (backendScript = await wsl([
    'wslpath', '-a', path.join(root, 'server/scripts/local-backend.sh')]));
  const backendAction = async action => wsl(['bash', await script(), action, String(config.backendPort)]);
  const adapter = {
    status: () => backendAction('status'),
    start: async () => {
      const result = await backendAction('start');
      if (result.startsWith('STARTED ')) state.backendStarts++;
      log(`Backend: ${result}`);
    },
    addresses: async () => (await wsl(['hostname', '-I'])).split(/\s+/).filter(net.isIPv4),
    probe: ip => getJson(`http://${ip}:${config.backendPort}/api/system/storage`, config.healthTimeoutMs)
  };
  let stopping = false, timer, lastMessage = '', failures = 0;
  async function stop() {
    if (stopping) return;
    stopping = true; clearTimeout(timer); state.ready = false; gateway.shutdown();
    try { log(await backendAction('stop')); } catch (error) { log(`Stop failed: ${error.message}`); }
    log('Launcher stopped');
  }
  process.on('SIGINT', stop); process.on('SIGTERM', stop);
  log(`Launcher started. Open ${address}; keep this window open.`);
  async function tick() {
    if (stopping) return;
    try {
      if (fs.existsSync(stopFile) && JSON.parse(fs.readFileSync(stopFile, 'utf8')).instance === state.instance)
        return await stop();
      try {
        if (!state.upstream || (await adapter.probe(state.upstream)).ok !== true) throw new Error('Not ready');
        state.ready = true; state.error = '';
      } catch (_) {
        state.ready = false; await recoverBackend(adapter, state);
      }
      failures = 0;
    } catch (error) { state.ready = false; state.error = error.message; failures++; }
    state.checkedAt = new Date().toISOString();
    const message = state.ready ? `READY ${address} -> ${state.upstream}:${config.backendPort}` : state.error;
    if (message !== lastMessage) { log(message); lastMessage = message; }
    if (!stopping) timer = setTimeout(tick, Math.min(30000,
      Math.max(1000, config.checkIntervalMs || 5000) * Math.max(1, failures)));
  }
  await tick();
}
module.exports = { getJson, createGateway, recoverBackend };
if (require.main === module) main().catch(error => { console.error(error.message); process.exitCode = 1; });
