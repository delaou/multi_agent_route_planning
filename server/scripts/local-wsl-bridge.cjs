// Temporary replacement for a stalled WSL localhost relay.
// Usage: node local-wsl-bridge.cjs <verified-WSL-IPv4>
// Local connections only. Stop this process before restoring native forwarding.
const net = require('node:net');
const upstreamHost = process.argv[2];
if (!net.isIPv4(upstreamHost)) throw new Error('A verified WSL IPv4 address is required');
const server = net.createServer(client => {
  const upstream = net.createConnection({ host: upstreamHost, port: 8080 });
  const close = () => { client.destroy(); upstream.destroy(); };
  upstream.setTimeout(65000, close);
  client.setTimeout(65000, close);
  upstream.on('error', close);
  client.on('error', close);
  upstream.on('close', () => client.destroy());
  client.on('close', () => upstream.destroy());
  client.pipe(upstream);
  upstream.pipe(client);
});
server.on('error', error => { console.error(error.message); process.exit(1); });
server.listen(8080, '127.0.0.1', () => console.log(`127.0.0.1:8080 -> ${upstreamHost}:8080`));
