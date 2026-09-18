#!/usr/bin/env node
'use strict';
/*
 * Read-only probe for the gated BlueBubbles transport candidate on the iPad.
 *
 *   BLUEBUBBLES_HOST=<ipad-lan-ip> \
 *   BLUEBUBBLES_PASSWORD=... \
 *   [BLUEBUBBLES_PORT=1234] \
 *   [BLUEBUBBLES_CERT_SHA256=<colon-separated fingerprint>] \
 *   [BLUEBUBBLES_SCHEME=http|https]   (http is the default transport) \
 *   node tests/live-bluebubbles-probe.js
 *
 * It never sends a message, never creates or mutates anything (the two
 * POSTs it makes, /chat/new with an empty body and /message/attachment
 * with a JSON body, are refused by validation before any bridge work),
 * and prints
 * only schema-level facts: status codes, counts, field presence, value
 * types, timings. Addresses, names, message text, guids, and the password
 * never reach stdout or stderr.
 *
 * TLS: the device certificate is self-signed. With BLUEBUBBLES_CERT_SHA256
 * set, the connection is refused unless the fingerprint matches. Without
 * it, the certificate is accepted once and its fingerprint is printed so
 * it can be pinned on the next run.
 */

const https = require('https');
const http = require('http');
const tls = require('tls');
const net = require('net');
const crypto = require('crypto');

const host = process.env.BLUEBUBBLES_HOST;
const password = process.env.BLUEBUBBLES_PASSWORD;
const port = Number(process.env.BLUEBUBBLES_PORT || 1234);
const pinned = (process.env.BLUEBUBBLES_CERT_SHA256 || '').replace(/[^0-9A-Fa-f]/g, '').toUpperCase();
const scheme = process.env.BLUEBUBBLES_SCHEME === 'https' ? 'https' : 'http';

if (!host || !password) {
  console.error('MISSING_BLUEBUBBLES_HOST_OR_PASSWORD');
  process.exit(2);
}
if (!Number.isInteger(port) || port < 1 || port > 65535) {
  console.error('INVALID_BLUEBUBBLES_PORT');
  process.exit(2);
}

const results = [];
let observedFingerprint = null;

function record(step, ok, facts) {
  results.push({ step, ok, ...facts });
  console.log(`${ok ? 'PASS' : 'FAIL'} ${step} ${JSON.stringify(facts)}`);
}

function checkServerIdentity(_hostname, certificate) {
  const fingerprint = (certificate.fingerprint256 || '').replace(/:/g, '').toUpperCase();
  observedFingerprint = fingerprint;
  if (pinned && fingerprint !== pinned) {
    return new Error('certificate fingerprint does not match BLUEBUBBLES_CERT_SHA256');
  }
  return undefined;
}

const agent = scheme === 'https' ?
  new https.Agent({ rejectUnauthorized: false, checkServerIdentity, keepAlive: true }) :
  new http.Agent({ keepAlive: true });
const client = scheme === 'https' ? https : http;

function request(method, path, { body, auth = true, headers = {} } = {}) {
  return new Promise((resolve, reject) => {
    const url = new URL(`${scheme}://${host}:${port}${path}`);
    if (auth) url.searchParams.set('guid', password);
    const payload = body === undefined ? null : Buffer.from(JSON.stringify(body));
    const started = process.hrtime.bigint();
    const req = client.request(url, {
      method,
      agent,
      headers: {
        ...(payload ? { 'Content-Type': 'application/json', 'Content-Length': payload.length } : {}),
        ...headers
      }
    }, res => {
      const chunks = [];
      res.on('data', chunk => chunks.push(chunk));
      res.on('end', () => {
        const raw = Buffer.concat(chunks);
        let json = null;
        try { json = raw.length ? JSON.parse(raw.toString('utf8')) : null; } catch { json = undefined; }
        resolve({
          status: res.statusCode,
          headers: res.headers,
          bytes: raw.length,
          json,
          ms: Number(process.hrtime.bigint() - started) / 1e6
        });
      });
    });
    req.on('error', reject);
    req.setTimeout(45000, () => req.destroy(new Error('timeout')));
    if (payload) req.write(payload);
    req.end();
  });
}

const typeOf = value => value === null ? 'null' : Array.isArray(value) ? 'array' : typeof value;

/* Field presence/type summary without values. */
function shape(object, keys) {
  const out = {};
  for (const key of keys) out[key] = object && Object.prototype.hasOwnProperty.call(object, key) ? typeOf(object[key]) : 'absent';
  return out;
}

function envelopeOk(response) {
  return response.json && typeof response.json === 'object' &&
    typeof response.json.status === 'number' && typeof response.json.message === 'string';
}

/* Guid pattern only: service prefix and direct/group marker. */
function guidPattern(guid) {
  if (typeof guid !== 'string') return 'not-a-string';
  const match = /^([A-Za-z]+);([-+]);(.+)$/.exec(guid);
  if (!match) return 'unstructured';
  return `${match[1]};${match[2]};<${match[3].startsWith('chat') ? 'chat-id' : 'address'}>`;
}

async function probeHttp() {
  let response = await request('OPTIONS', '/api/v1/chat/query', { auth: false });
  record('options', response.status === 204,
    { status: response.status, cors: response.headers['access-control-allow-origin'] === '*', ms: Math.round(response.ms) });

  response = await request('GET', '/api/v1/ping', { auth: false });
  record('ping-unauthorized', response.status === 401 && envelopeOk(response),
    { status: response.status, errorType: response.json && response.json.error && response.json.error.type });

  response = await request('GET', '/api/v1/ping');
  record('ping', response.status === 200 && envelopeOk(response) && response.json.data === 'pong',
    { status: response.status, ms: Math.round(response.ms) });

  response = await request('GET', '/api/v1/server/info');
  const info = envelopeOk(response) ? response.json.data : null;
  record('server-info', response.status === 200 && info && info.server_version === '0.2.0' && info.private_api === false,
    { status: response.status, ...shape(info, ['os_version', 'server_version', 'private_api', 'local_ipv4s', 'ios_capabilities']),
      localAddressCount: info && Array.isArray(info.local_ipv4s) ? info.local_ipv4s.length : null });

  response = await request('GET', '/api/v1/fcm/client');
  record('fcm-client', response.status === 404 && envelopeOk(response), { status: response.status });

  response = await request('GET', '/api/v1/chat/count');
  const count = envelopeOk(response) ? response.json.data : null;
  record('chat-count', response.status === 200 && count && typeof count.total === 'number',
    { status: response.status, total: count && count.total, breakdown: count && count.breakdown, ms: Math.round(response.ms) });

  response = await request('POST', '/api/v1/chat/query', {
    body: { with: ['participants', 'lastmessage'], offset: 0, limit: 100, sort: 'lastmessage' }
  });
  const chats = envelopeOk(response) && Array.isArray(response.json.data) ? response.json.data : null;
  const chatFacts = { status: response.status, bytes: response.bytes, ms: Math.round(response.ms),
    metadata: response.json && response.json.metadata, count: chats ? chats.length : null };
  if (chats && chats.length) {
    const styles = {};
    const patterns = {};
    let withParticipants = 0;
    let withLastMessage = 0;
    let zeroRowId = 0;
    for (const chat of chats) {
      styles[chat.style] = (styles[chat.style] || 0) + 1;
      const pattern = guidPattern(chat.guid);
      patterns[pattern] = (patterns[pattern] || 0) + 1;
      if (Array.isArray(chat.participants) && chat.participants.length) withParticipants++;
      if (chat.lastMessage && typeof chat.lastMessage === 'object') withLastMessage++;
      if (!chat.originalROWID) zeroRowId++;
      for (const participant of chat.participants || []) if (!participant.originalROWID) zeroRowId++;
    }
    Object.assign(chatFacts, { styles, guidPatterns: patterns, withParticipants, withLastMessage, zeroRowId,
      firstChatShape: shape(chats[0], ['originalROWID', 'guid', 'style', 'chatIdentifier', 'isArchived', 'displayName', 'participants', 'lastMessage']),
      firstParticipantShape: shape((chats[0].participants || [])[0], ['originalROWID', 'address', 'service', 'country', 'uncanonicalizedId']) });
  }
  record('chat-query', response.status === 200 && chats !== null, chatFacts);
  if (!chats || !chats.length) return;

  /* One history request, the way the client asks during full sync. */
  const target = chats[0];
  response = await request('GET', `/api/v1/chat/${encodeURIComponent(target.guid)}/message?with=attachments,handle&sort=DESC&after=0&offset=0&limit=25`);
  const messages = envelopeOk(response) && Array.isArray(response.json.data) ? response.json.data : null;
  const messageFacts = { status: response.status, bytes: response.bytes, ms: Math.round(response.ms),
    metadata: response.json && response.json.metadata, count: messages ? messages.length : null };
  if (messages && messages.length) {
    let fromMe = 0, withHandle = 0, withAttachments = 0, delivered = 0, read = 0, textNonEmpty = 0, sortedDesc = true, zeroRowId = 0;
    for (let i = 0; i < messages.length; i++) {
      const message = messages[i];
      if (message.isFromMe) fromMe++;
      if (message.handle) withHandle++;
      if (Array.isArray(message.attachments) && message.attachments.length) withAttachments++;
      if (message.dateDelivered) delivered++;
      if (message.dateRead) read++;
      if (typeof message.text === 'string' && message.text.length) textNonEmpty++;
      if (!message.originalROWID) zeroRowId++;
      if (i && messages[i - 1].dateCreated < message.dateCreated) sortedDesc = false;
    }
    Object.assign(messageFacts, { fromMe, withHandle, withAttachments, delivered, read, textNonEmpty, sortedDesc, zeroRowId,
      dateCreatedType: typeOf(messages[0].dateCreated),
      dateLooksLikeMs: typeof messages[0].dateCreated === 'number' && messages[0].dateCreated > 1e12,
      firstMessageShape: shape(messages[0], ['originalROWID', 'guid', 'text', 'handle', 'handleId', 'attachments', 'subject', 'error',
        'dateCreated', 'dateRead', 'dateDelivered', 'isDelivered', 'isFromMe', 'itemType', 'hasAttachments']) });
    const attachment = messages.map(m => (m.attachments || [])[0]).find(Boolean);
    if (attachment) messageFacts.firstAttachmentShape = shape(attachment, ['originalROWID', 'guid', 'uti', 'mimeType', 'transferName', 'totalBytes', 'transferState', 'isOutgoing']);
  }
  record('chat-history', response.status === 200 && messages !== null, messageFacts);

  response = await request('GET', `/api/v1/chat/${encodeURIComponent(target.guid)}`);
  record('chat-get', response.status === 200 && envelopeOk(response) && response.json.data && response.json.data.guid === target.guid,
    { status: response.status, ms: Math.round(response.ms) });

  const weekAgo = Date.now() - 7 * 24 * 3600 * 1000;
  response = await request('GET', `/api/v1/message/count?after=${weekAgo}`);
  record('message-count-7d', response.status === 200 && envelopeOk(response) && response.json.data && typeof response.json.data.total === 'number',
    { status: response.status, total: response.json && response.json.data && response.json.data.total, ms: Math.round(response.ms) });

  response = await request('POST', '/api/v1/message/query', {
    body: { with: ['chats', 'chats.participants', 'attachments', 'handle'], where: [], sort: 'DESC', after: weekAgo, before: null, chatGuid: null, offset: 0, limit: 5 }
  });
  const recent = envelopeOk(response) && Array.isArray(response.json.data) ? response.json.data : null;
  record('message-query-7d', response.status === 200 && recent !== null,
    { status: response.status, count: recent ? recent.length : null, ms: Math.round(response.ms),
      withChats: recent ? recent.filter(m => Array.isArray(m.chats) && m.chats.length).length : null,
      metadata: response.json && response.json.metadata });

  response = await request('GET', '/api/v1/chat/does-not-exist-fixture');
  record('chat-get-missing', response.status === 404 && envelopeOk(response),
    { status: response.status, errorType: response.json && response.json.error && response.json.error.type });

  /* New-chat route presence, without creating anything: an empty body must
   * be refused before it reaches the bridge (400 Validation Error). A
   * route that is absent answers 404/405 instead. Nothing is sent. */
  response = await request('POST', '/api/v1/chat/new', { body: {} });
  record('chat-new-rejects-empty', response.status === 400 && envelopeOk(response) &&
    response.json.error && response.json.error.type === 'Validation Error',
    { status: response.status, errorType: response.json && response.json.error && response.json.error.type,
      ms: Math.round(response.ms) });

  /* Attachment-send route presence, without sending: a JSON body is not
   * multipart, so the router drains it and answers 400 before any file
   * is staged or any bridge work happens. An older build answers 405. */
  response = await request('POST', '/api/v1/message/attachment', { body: {} });
  record('attachment-send-rejects-json', response.status === 400 && envelopeOk(response) &&
    response.json.error && response.json.error.type === 'Validation Error',
    { status: response.status, errorType: response.json && response.json.error && response.json.error.type,
      ms: Math.round(response.ms) });

  /* Contacts: names + phones/emails from the device AddressBook. */
  response = await request('GET', '/api/v1/contact?extraProperties=avatar');
  const contacts = envelopeOk(response) && Array.isArray(response.json.data) ? response.json.data : null;
  const contactFacts = { status: response.status, count: contacts ? contacts.length : null, ms: Math.round(response.ms) };
  if (contacts && contacts.length) {
    let withName = 0, withPhone = 0, withEmail = 0, withAvatar = 0;
    for (const contact of contacts) {
      if ((contact.displayName || contact.firstName || contact.lastName)) withName++;
      if (Array.isArray(contact.phoneNumbers) && contact.phoneNumbers.length) withPhone++;
      if (Array.isArray(contact.emails) && contact.emails.length) withEmail++;
      if (contact.avatar) withAvatar++;
    }
    Object.assign(contactFacts, { withName, withPhone, withEmail, withAvatar,
      firstShape: shape(contacts[0], ['id', 'displayName', 'firstName', 'lastName', 'phoneNumbers', 'emails', 'avatar']) });
  }
  record('contacts', response.status === 200 && contacts !== null, contactFacts);

  /* Attachment download: raw bytes, not JSON. Look through history/recent,
   * then a few more chats (bounded), for any attachment guid; skips (still
   * ok) only when none of the sampled chats has an attachment. */
  const guids = new Set();
  const collect = lists => {
    for (const list of lists) for (const message of list || []) {
      for (const attachment of message.attachments || []) if (attachment.guid) guids.add(attachment.guid);
    }
  };
  collect([messages, recent]);
  for (let i = 1; guids.size === 0 && i < Math.min(chats.length, 6); i++) {
    const scan = await request('GET', `/api/v1/chat/${encodeURIComponent(chats[i].guid)}/message?with=attachments,handle&limit=25`);
    collect([envelopeOk(scan) && Array.isArray(scan.json.data) ? scan.json.data : null]);
  }
  if (guids.size === 0) {
    record('attachment-download', true, { skipped: 'no attachment in sampled window' });
  } else {
    /* Try each until one is downloaded (raw bytes); a truthful 404 for an
     * undownloaded transfer is also a correct outcome. */
    let facts = null;
    for (const guid of guids) {
      response = await request('GET', `/api/v1/attachment/${encodeURIComponent(guid)}/download?original=false`);
      const contentType = response.headers['content-type'] || '';
      const isJson = contentType.includes('application/json');
      const downloaded = response.status === 200 && response.bytes > 0 && !isJson;
      const notDownloaded = response.status === 404 && isJson;
      facts = { status: response.status, bytes: response.bytes, tried: guids.size,
        result: downloaded ? 'raw-bytes' : notDownloaded ? 'not-downloaded-404' : 'unexpected',
        ok: downloaded || notDownloaded };
      if (downloaded) break;
    }
    record('attachment-download', facts.ok, facts);
  }
}

/* Minimal RFC 6455 client over TLS for the Socket.IO handshake. */
function probeSocketIO() {
  /* Isolated so it can only ever resolve: no throw here can reject the outer
   * chain into an opaque probe-error. Every stage is labelled so a failure
   * reports where and why. */
  return new Promise(resolve => {
    const started = Date.now();
    const facts = { stage: 'setup' };
    let finished = false;
    let socket = null;
    const finish = ok => {
      if (finished) return;
      finished = true;
      facts.ms = Date.now() - started;
      record('socket-io', ok, facts);
      try { if (socket) socket.end(); } catch { /* ignore */ }
      resolve();
    };
    const fail = (stage, error) => {
      facts.stage = stage;
      if (error) facts.error = error.code || error.message;
      finish(false);
    };

    let timer = null;
    let buffer = Buffer.alloc(0);
    let upgraded = false;
    const packets = [];

    const sendText = text => {
      const payload = Buffer.from(text, 'utf8');
      const mask = crypto.randomBytes(4);
      const header = payload.length < 126 ? Buffer.from([0x81, 0x80 | payload.length]) :
        Buffer.from([0x81, 0x80 | 126, (payload.length >> 8) & 0xff, payload.length & 0xff]);
      const masked = Buffer.alloc(payload.length);
      for (let i = 0; i < payload.length; i++) masked[i] = payload[i] ^ mask[i % 4];
      socket.write(Buffer.concat([header, mask, masked]));
    };

    try {
      /* SNI servername must be a hostname, never an IP address. */
      const isIp = net.isIP(host) !== 0;
      const options = { host, port, rejectUnauthorized: false, checkServerIdentity };
      if (scheme === 'https' && !isIp) options.servername = host;
      socket = scheme === 'https' ? tls.connect(options) : net.connect({ host, port });
    } catch (error) {
      return fail('connect', error);
    }
    timer = setTimeout(() => fail('timeout'), 20000);

    socket.on(scheme === 'https' ? 'secureConnect' : 'connect', () => {
      try {
        facts.stage = 'handshake';
        /* checkServerIdentity is skipped under rejectUnauthorized:false, so
         * capture the fingerprint here for pinning on the next run. */
        if (scheme === 'https' && typeof socket.getPeerCertificate === 'function') {
          const cert = socket.getPeerCertificate();
          if (cert && cert.fingerprint256) {
            observedFingerprint = cert.fingerprint256.replace(/:/g, '').toUpperCase();
            if (pinned && observedFingerprint !== pinned) return fail('cert-pin-mismatch');
          }
        }
        const key = crypto.randomBytes(16).toString('base64');
        facts.expectedAccept = crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
        socket.write(`GET /socket.io/?EIO=4&transport=websocket&guid=${encodeURIComponent(password)} HTTP/1.1\r\n` +
          `Host: ${host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ${key}\r\n\r\n`);
      } catch (error) {
        fail('handshake-write', error);
      }
    });

    socket.on('data', chunk => {
     try {
      buffer = Buffer.concat([buffer, chunk]);
      if (!upgraded) {
        const end = buffer.indexOf('\r\n\r\n');
        if (end < 0) return;
        const head = buffer.slice(0, end).toString('latin1');
        buffer = buffer.slice(end + 4);
        facts.status = Number((/^HTTP\/1\.1 (\d+)/.exec(head) || [])[1]);
        const accept = (/Sec-WebSocket-Accept: (\S+)/i.exec(head) || [])[1];
        facts.acceptMatches = accept === facts.expectedAccept;
        delete facts.expectedAccept;
        if (facts.status !== 101) return finish(false);
        upgraded = true;
      }
      for (;;) {
        if (buffer.length < 2) return;
        const opcode = buffer[0] & 0x0f;
        let length = buffer[1] & 0x7f;
        let offset = 2;
        if (length === 126) { if (buffer.length < 4) return; length = buffer.readUInt16BE(2); offset = 4; }
        else if (length === 127) { facts.frameTooLarge = true; return finish(false); }
        if (buffer.length < offset + length) return;
        const payload = buffer.slice(offset, offset + length).toString('utf8');
        buffer = buffer.slice(offset + length);
        if (opcode === 0x8) { facts.serverClosed = true; return finish(packets.length >= 3); }
        if (opcode !== 0x1) continue;
        packets.push(payload);
        if (packets.length === 1) {
          let open = null;
          try { open = JSON.parse(payload.slice(1)); } catch { /* ignore */ }
          facts.open = payload[0] === '0' && !!open && typeof open.sid === 'string' &&
            open.pingInterval === 60000 && open.pingTimeout === 120000;
          if (!facts.open) return finish(false);
          sendText('40');
        } else if (packets.length === 2) {
          facts.namespaceConnected = /^40\{"sid":"[^"]+"\}$/.test(payload);
          if (!facts.namespaceConnected) return finish(false);
          sendText('4212["get-server-metadata",{}]');
        } else if (packets.length === 3) {
          facts.ackReceived = payload.startsWith('4312[');
          let ack = null;
          try { ack = JSON.parse(payload.slice(4)); } catch { /* ignore */ }
          facts.ackEnvelope = Array.isArray(ack) && ack.length === 1 && ack[0].status === 200 &&
            ack[0].data && ack[0].data.server_version === '0.2.0';
          sendText('41');
          setTimeout(() => finish(facts.ackReceived && facts.ackEnvelope), 500);
        }
      }
     } catch (error) {
      fail(`data-parse@packet${packets.length}`, error);
     }
    });
    socket.on('error', error => { if (timer) clearTimeout(timer); fail(facts.stage, error); });
    socket.on('close', () => { if (timer) clearTimeout(timer); finish(packets.length >= 3 && facts.ackEnvelope === true); });
  });
}

(async () => {
  try {
    await probeHttp();
    await probeSocketIO();
  } catch (error) {
    record('probe-error', false, { error: error.code || error.message });
  }
  agent.destroy();
  const failed = results.filter(result => !result.ok).length;
  console.log(JSON.stringify({
    summary: { steps: results.length, failed },
    certificateFingerprintSha256: observedFingerprint,
    pinned: pinned ? 'enforced' : 'not-enforced (set BLUEBUBBLES_CERT_SHA256 to pin)'
  }));
  process.exit(failed ? 1 : 0);
})();
