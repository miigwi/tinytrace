// A local harness that serves the Worker over plain HTTP, so the protocol can
// be exercised with curl before anything is deployed or flashed.
//
// This is not the deploy path — `wrangler dev` is, and it runs the real
// workerd. But wrangler is a large install, and the point of building the relay
// first is that the protocol can be poked at with nothing but the tools already
// on the machine. Node 22 has Request/Response/fetch built in, so the Worker
// module runs here unmodified.
//
//   FLEET_KEY=dev-key node dev/serve.js
//   curl -sS localhost:8787/health
//
// With no DT_URL set, forwarding fails and is logged — which is itself the
// interesting demonstration: the device still gets its 202.

import { createServer } from 'node:http';

import worker from '../src/index.js';

const port = Number(process.env.PORT || 8787);
const env = {
  FLEET_KEY: process.env.FLEET_KEY || 'dev-key',
  DT_URL: process.env.DT_URL || '',
  DT_TOKEN: process.env.DT_TOKEN || '',
};

// Mirror the Workers contract: waitUntil work runs after the response is sent,
// so a slow forward cannot hold the device's connection open.
const ctx = {
  waitUntil(promise) {
    Promise.resolve(promise).catch((err) => console.error(`waitUntil: ${err?.stack || err}`));
  },
};

const server = createServer(async (req, res) => {
  const chunks = [];
  for await (const chunk of req) chunks.push(chunk);
  const body = Buffer.concat(chunks);

  const request = new Request(`http://localhost:${port}${req.url}`, {
    method: req.method,
    headers: req.headers,
    body: body.length ? body : undefined,
  });

  const response = await worker.fetch(request, env, ctx);
  const text = await response.text();
  console.log(`${req.method} ${req.url} -> ${response.status}`);
  res.writeHead(response.status, Object.fromEntries(response.headers));
  res.end(text);
});

server.listen(port, () => {
  console.log(`relay on http://localhost:${port} (fleet key: ${env.FLEET_KEY})`);
  if (!env.DT_URL) console.log('DT_URL unset — reports are accepted, forwarding will fail and log');
});
