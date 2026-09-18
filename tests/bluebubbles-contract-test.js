'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const http = require('node:http');
const net = require('node:net');
const test = require('node:test');

const {
  RECORD_SEPARATOR,
  createFixtureServer,
  parseMultipart,
  parseSocketEventPacket
} = require('./bluebubbles-fixture-server');

let fixture;
let origin;

test.before(async () => {
  fixture = createFixtureServer();
  origin = await fixture.start();
});

test.after(async () => {
  await fixture.stop();
});

async function jsonRequest(path, options = {}) {
  const response = await fetch(`${origin}${path}`, options);
  return { response, body: await response.json() };
}

async function waitFor(predicate, timeoutMs = 1000) {
  const deadline = Date.now() + timeoutMs;
  while (!predicate()) {
    if (Date.now() >= deadline) throw new Error('Timed out waiting for fixture state');
    await new Promise(resolve => setTimeout(resolve, 5));
  }
}

function assertEnvelope(body, status = 200) {
  assert.equal(body.status, status);
  assert.equal(typeof body.message, 'string');
  if (status === 200) assert.ok(Object.hasOwn(body, 'data'));
  else {
    assert.equal(typeof body.error, 'object');
    assert.equal(typeof body.error.type, 'string');
    assert.equal(typeof body.error.message, 'string');
  }
}

function assertHandle(handle) {
  assert.ok(Number.isInteger(handle.originalROWID) && handle.originalROWID > 0);
  assert.equal(typeof handle.address, 'string');
  assert.ok(['iMessage', 'SMS'].includes(handle.service));
  assert.ok(Object.hasOwn(handle, 'country'));
  assert.ok(Object.hasOwn(handle, 'uncanonicalizedId'));
}

function assertAttachment(attachment) {
  for (const field of ['guid', 'uti', 'mimeType', 'transferName']) {
    assert.equal(typeof attachment[field], 'string', field);
  }
  assert.ok(Number.isInteger(attachment.originalROWID));
  assert.ok(Number.isInteger(attachment.totalBytes));
  assert.equal(typeof attachment.hasLivePhoto, 'boolean');
}

function assertChat(chat) {
  for (const field of ['guid', 'chatIdentifier', 'displayName']) {
    assert.equal(typeof chat[field], 'string', field);
  }
  assert.ok(Number.isInteger(chat.originalROWID));
  assert.ok([43, 45].includes(chat.style));
  assert.equal(typeof chat.isArchived, 'boolean');
  assert.ok(Array.isArray(chat.participants));
  chat.participants.forEach(assertHandle);
}

// nested: a message inside a chat (lastMessage, /chat/new messages) has
// no chats expansion, as the reference serializer omits it there.
function assertMessage(message, { nested = false } = {}) {
  const stringFields = ['guid', 'text'];
  stringFields.forEach(field => assert.equal(typeof message[field], 'string', field));
  assert.ok(message.subject === null || typeof message.subject === 'string', 'subject');
  for (const field of ['originalROWID', 'handleId', 'otherHandle', 'error', 'dateCreated', 'itemType',
    'groupActionType', 'partCount']) {
    assert.ok(Number.isInteger(message[field]), field);
  }
  assert.equal(typeof message.isFromMe, 'boolean');
  assert.equal(typeof message.isArchived, 'boolean');
  assert.ok(Array.isArray(message.attachments));
  message.attachments.forEach(assertAttachment);
  if (nested) {
    assert.ok(!Object.hasOwn(message, 'chats'), 'nested messages do not expand chats');
  } else {
    assert.ok(Array.isArray(message.chats));
    message.chats.forEach(assertChat);
  }
  if (message.handle !== null) assertHandle(message.handle);
  assert.ok(message.dateCreated > 1000000000000, 'dateCreated must be Unix milliseconds');
}

test('REST auth accepts guid/password/token aliases and rejects missing or wrong credentials', async () => {
  for (const alias of ['guid', 'password', 'token']) {
    const { response, body } = await jsonRequest(`/api/v1/ping?${alias}=fixture-only`);
    assert.equal(response.status, 200);
    assertEnvelope(body);
    assert.equal(body.data, 'pong');
  }

  for (const query of ['', '?guid=wrong']) {
    const { response, body } = await jsonRequest(`/api/v1/ping${query}`);
    assert.equal(response.status, 401);
    assertEnvelope(body, 401);
    assert.equal(body.error.type, 'Authentication Error');
  }
});

test('minimum stock-client startup and sync routes return deterministic BlueBubbles schemas', async () => {
  const auth = 'guid=fixture-only';

  const info = await jsonRequest(`/api/v1/server/info?${auth}`);
  assert.equal(info.response.status, 200);
  assertEnvelope(info.body);
  assert.equal(info.body.data.server_version, '0.2.0');
  assert.equal(info.body.data.os_version, '9.3.5');
  assert.equal(info.body.data.private_api, false);
  assert.deepEqual(info.body.data.local_ipv4s, ['127.0.0.1']);

  const fcm = await jsonRequest(`/api/v1/fcm/client?${auth}`);
  assert.equal(fcm.response.status, 404);
  assertEnvelope(fcm.body, 404);
  assert.equal(fcm.body.error.type, 'Capability Error');

  const count = await jsonRequest(`/api/v1/chat/count?${auth}`);
  assertEnvelope(count.body);
  assert.equal(count.body.data.total, 2);

  const chats = await jsonRequest(`/api/v1/chat/query?${auth}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ with: ['lastMessage'], offset: 0, limit: 100, sort: null })
  });
  assertEnvelope(chats.body);
  assert.equal(chats.body.metadata.total, 2);
  assert.equal(chats.body.metadata.count, 2);
  chats.body.data.forEach(chat => {
    assertChat(chat);
    assertMessage(chat.lastMessage);
  });
  assert.deepEqual(chats.body.data.map(chat => chat.guid), [
    'iMessage;-;+15555550100',
    'iMessage;+;fixture-group-1'
  ]);

  for (const chat of chats.body.data) {
    const path = `/api/v1/chat/${encodeURIComponent(chat.guid)}/message?${auth}` +
      '&with=attachments,handle,message.attributedBody,message.messageSummaryInfo,message.payloadData' +
      '&sort=DESC&after=0&offset=0&limit=25';
    const history = await jsonRequest(path);
    assertEnvelope(history.body);
    assert.ok(history.body.data.length > 0);
    history.body.data.forEach(assertMessage);
    const dates = history.body.data.map(message => message.dateCreated);
    assert.deepEqual(dates, dates.slice().sort((a, b) => b - a));
  }

  const messageCount = await jsonRequest(`/api/v1/message/count?${auth}&after=0`);
  assertEnvelope(messageCount.body);
  assert.equal(messageCount.body.data.total, 4);

  const updatedCount = await jsonRequest(`/api/v1/message/count/updated?${auth}&after=0`);
  assertEnvelope(updatedCount.body);
  assert.equal(updatedCount.body.data.total, 4);

  const messages = await jsonRequest(`/api/v1/message/query?${auth}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      with: ['chats', 'chats.participants', 'attachments', 'handle', 'attributedBody',
        'messageSummaryInfo', 'payloadData'],
      where: [], sort: 'DESC', after: 0, before: 1800000000000,
      chatGuid: null, offset: 0, limit: 100, convertAttachments: true
    })
  });
  assertEnvelope(messages.body);
  assert.equal(messages.body.metadata.total, 4);
  messages.body.data.forEach(assertMessage);
  assert.equal(new Set(messages.body.data.map(message => message.guid)).size, 4);
  assert.ok(messages.body.data.some(message => message.isDelivered && message.dateRead !== null));
});

test('attachment metadata, raw download, ranges, and missing transfers remain truthful', async () => {
  const auth = 'guid=fixture-only';
  const guid = 'fixture-attachment-downloaded';
  const metadata = await jsonRequest(`/api/v1/attachment/${guid}?${auth}`);
  assertEnvelope(metadata.body);
  assertAttachment(metadata.body.data);

  const full = await fetch(`${origin}/api/v1/attachment/${guid}/download?${auth}`);
  assert.equal(full.status, 200);
  assert.equal(full.headers.get('content-type'), 'image/png');
  assert.equal(Buffer.from(await full.arrayBuffer()).toString(), 'fixture-attachment-bytes');

  const partial = await fetch(`${origin}/api/v1/attachment/${guid}/download?${auth}`, {
    headers: { Range: 'bytes=8-17' }
  });
  assert.equal(partial.status, 206);
  assert.equal(partial.headers.get('content-range'), 'bytes 8-17/24');
  assert.equal(Buffer.from(await partial.arrayBuffer()).toString(), 'attachment');

  const missing = await jsonRequest(`/api/v1/attachment/fixture-attachment-missing/download?${auth}`);
  assert.equal(missing.response.status, 404);
  assertEnvelope(missing.body, 404);
  assert.match(missing.body.error.message, /not downloaded/i);
});

test('malformed JSON, multipart framing, and pagination return bounded validation errors', async () => {
  const malformedJson = await jsonRequest('/api/v1/chat/query?guid=fixture-only', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: '{"with":['
  });
  assert.equal(malformedJson.response.status, 400);
  assertEnvelope(malformedJson.body, 400);
  assert.equal(malformedJson.body.error.type, 'Validation Error');

  const malformedMultipart = await jsonRequest('/api/v1/attachment/upload?guid=fixture-only', {
    method: 'POST',
    headers: { 'Content-Type': 'multipart/form-data; boundary=fixture-boundary' },
    body: '--fixture-boundary\r\nContent-Disposition: form-data; name="attachment"; filename="x"\r\n\r\nabc'
  });
  assert.equal(malformedMultipart.response.status, 400);
  assertEnvelope(malformedMultipart.body, 400);

  const form = new FormData();
  form.append('attachment', new Blob([Buffer.from('synthetic upload')]), 'fixture.txt');
  const validMultipart = await jsonRequest('/api/v1/attachment/upload?guid=fixture-only', {
    method: 'POST',
    body: form
  });
  assert.equal(validMultipart.response.status, 200);
  assertEnvelope(validMultipart.body);
  assert.equal(validMultipart.body.data.path, 'fixture-upload/fixture.txt');

  assert.throws(() => parseMultipart('multipart/form-data', Buffer.from('x')), /boundary/i);

  const badPagination = await jsonRequest('/api/v1/chat/query?guid=fixture-only', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ offset: -1, limit: 0 })
  });
  assert.equal(badPagination.response.status, 400);
  assertEnvelope(badPagination.body, 400);

  const preflight = await fetch(`${origin}/api/v1/chat/query`, { method: 'OPTIONS' });
  assert.equal(preflight.status, 204);
  assert.equal(preflight.headers.get('access-control-allow-origin'), '*');
});

test('POST /chat/new and start-chat create a chat with its first message and refuse iOS 9 gaps', async () => {
  const created = await jsonRequest('/api/v1/chat/new?guid=fixture-only', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ addresses: ['+15555550199'], message: 'first', service: 'iMessage', method: 'apple-script' })
  });
  assert.equal(created.response.status, 200);
  assertEnvelope(created.body);
  assert.equal(created.body.message, 'Successfully created chat!');
  assertChat(created.body.data);
  assert.equal(created.body.data.guid, 'iMessage;-;+15555550199');
  assert.equal(created.body.data.style, 45);
  assert.equal(created.body.data.participants.length, 1);
  assert.ok(Array.isArray(created.body.data.messages));
  assert.equal(created.body.data.messages.length, 1);
  assertMessage(created.body.data.messages[0], { nested: true });
  assert.equal(created.body.data.messages[0].isFromMe, true);
  assert.equal(created.body.data.messages[0].text, 'first');
  assert.ok(!Object.hasOwn(created.body.data.messages[0], 'tempGuid'));

  const group = await jsonRequest('/api/v1/chat/new?guid=fixture-only', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ addresses: ['+15555550198', 'a@example.invalid'], message: 'hi', service: 'sms', tempGuid: 'temp-42' })
  });
  assert.equal(group.response.status, 200);
  assertChat(group.body.data);
  assert.equal(group.body.data.style, 43);
  assert.match(group.body.data.guid, /^SMS;\+;/);
  assert.equal(group.body.data.participants.length, 2);
  assert.equal(group.body.data.messages[0].tempGuid, 'temp-42');

  const rejected = [
    {},
    { addresses: [], message: 'x' },
    { addresses: [1], message: 'x' },
    { addresses: [''], message: 'x' },
    { addresses: ['+1'] },
    { addresses: ['+1'], message: '' },
    { addresses: ['+1'], message: 'x', service: 'RCS' },
    { addresses: ['+1'], message: 'x', effectId: 'slam' },
    { addresses: ['+1'], message: 'x', subject: 's' },
    { addresses: ['+1'], message: 'x', attributedBody: {} },
    { addresses: ['1', '2', '3', '4', '5', '6', '7', '8', '9'], message: 'x' }
  ];
  for (const body of rejected) {
    const reply = await jsonRequest('/api/v1/chat/new?guid=fixture-only', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    });
    assert.equal(reply.response.status, 400, JSON.stringify(body));
    assertEnvelope(reply.body, 400);
    assert.equal(reply.body.error.type, 'Validation Error');
  }
  const missingMessage = await jsonRequest('/api/v1/chat/new?guid=fixture-only', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ addresses: ['+1'] })
  });
  assert.equal(missingMessage.body.error.message, 'A message is required to create a chat on iOS 9');
  const wrongMethod = await fetch(`${origin}/api/v1/chat/new?guid=fixture-only`);
  assert.equal(wrongMethod.status, 405);

  const client = await RawWebSocketClient.connect(Number(new URL(origin).port));
  try {
    await client.readText();
    client.sendText('40');
    await client.readText();
    client.sendText('42901["start-chat",{"participants":"+15555550197","message":"yo","tempGuid":"t-1"}]');
    const ack = await client.readText();
    assert.match(ack, /^43901\[/);
    const [envelope] = JSON.parse(ack.slice('43901'.length));
    assertEnvelope(envelope);
    assertChat(envelope.data);
    assert.equal(envelope.data.guid, 'iMessage;-;+15555550197');
    assert.equal(envelope.data.messages[0].tempGuid, 't-1');
    client.sendText('42902["start-chat",{"participants":["a@example.invalid","b@example.invalid"],"message":"yo"}]');
    const [groupEnvelope] = JSON.parse((await client.readText()).slice('43902'.length));
    assert.equal(groupEnvelope.data.style, 43);
    client.sendText('42903["start-chat",{"participants":[]}]');
    const [rejectedEnvelope] = JSON.parse((await client.readText()).slice('43903'.length));
    assertEnvelope(rejectedEnvelope, 400);
    client.sendText('42904["start-chat",{"participants":"+1"}]');
    const [noMessage] = JSON.parse((await client.readText()).slice('43904'.length));
    assert.equal(noMessage.status, 400);
    assert.match(noMessage.error.message, /message is required/);
  } finally {
    await client.close();
  }
});

test('POST /message/attachment accepts the stock multipart send and refuses iOS 9 gaps', async () => {
  const bytes = Buffer.alloc(70000);
  for (let i = 0; i < bytes.length; i++) bytes[i] = (i * 13 + 5) & 0xff;
  const send = async (fields, { file = bytes, filename = 'pic.jpg', guid = 'fixture-only' } = {}) => {
    const form = new FormData();
    if (file) form.append('attachment', new Blob([file]), filename);
    for (const [key, value] of Object.entries(fields)) form.append(key, value);
    return jsonRequest(`/api/v1/message/attachment?guid=${guid}`, { method: 'POST', body: form });
  };
  const sent = await send({ chatGuid: 'iMessage;-;+15555550100', tempGuid: 'temp-a1', name: 'pic.jpg', method: 'apple-script' });
  assert.equal(sent.response.status, 200);
  assertEnvelope(sent.body);
  assert.equal(sent.body.message, 'Attachment sent!');
  assertMessage(sent.body.data);
  assert.equal(sent.body.data.tempGuid, 'temp-a1');
  assert.equal(sent.body.data.isFromMe, true);
  assert.equal(sent.body.data.attachments.length, 1);
  assert.equal(sent.body.data.attachments[0].totalBytes, 70000);
  assert.equal(sent.body.data.attachments[0].transferName, 'pic.jpg');

  const rejected = [
    [{ tempGuid: 't', name: 'x' }, 400],
    [{ chatGuid: 'iMessage;-;+15555550100', name: 'x' }, 400],
    [{ chatGuid: 'iMessage;-;+15555550100', tempGuid: 't', effectId: 'slam' }, 400],
    [{ chatGuid: 'iMessage;-;+15555550100', tempGuid: 't', subject: 's' }, 400],
    [{ chatGuid: 'iMessage;-;+15555550100', tempGuid: 't', selectedMessageGuid: 'g' }, 400],
    [{ chatGuid: 'iMessage;-;does-not-exist', tempGuid: 't' }, 404]
  ];
  for (const [fields, status] of rejected) {
    const reply = await send(fields);
    assert.equal(reply.response.status, status, JSON.stringify(fields));
    assertEnvelope(reply.body, status);
  }
  const noFile = await send({ chatGuid: 'iMessage;-;+15555550100', tempGuid: 't' }, { file: null });
  assert.equal(noFile.response.status, 400);
  const unauthorized = await send({ chatGuid: 'iMessage;-;+15555550100', tempGuid: 't' }, { guid: 'wrong' });
  assert.equal(unauthorized.response.status, 401);
  const notMultipart = await jsonRequest('/api/v1/message/attachment?guid=fixture-only', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: '{}'
  });
  assert.equal(notMultipart.response.status, 400);
  assertEnvelope(notMultipart.body, 400);
  assert.equal(notMultipart.body.error.message, 'Expected a multipart/form-data body');
  const wrongMethod = await fetch(`${origin}/api/v1/message/attachment?guid=fixture-only`);
  assert.equal(wrongMethod.status, 405);
});

test('chunked request bodies and simultaneous REST requests do not share request state', async () => {
  const port = new URL(origin).port;
  const chunked = await new Promise((resolve, reject) => {
    const req = http.request({
      host: '127.0.0.1', port, method: 'POST',
      path: '/api/v1/chat/query?guid=fixture-only',
      headers: { 'Content-Type': 'application/json' }
    }, res => {
      const chunks = [];
      res.on('data', chunk => chunks.push(chunk));
      res.on('end', () => resolve({ status: res.statusCode, body: JSON.parse(Buffer.concat(chunks)) }));
    });
    req.on('error', reject);
    req.write('{"with":["participants"],');
    req.write('"offset":0,"limit":1}');
    req.end();
  });
  assert.equal(chunked.status, 200);
  assertEnvelope(chunked.body);
  assert.equal(chunked.body.data.length, 1);

  const requests = Array.from({ length: 12 }, (_, index) =>
    jsonRequest(`/api/v1/ping?guid=fixture-only&delay=${(11 - index) % 10}`));
  const results = await Promise.all(requests);
  assert.equal(results.length, 12);
  results.forEach(({ response, body }) => {
    assert.equal(response.status, 200);
    assert.equal(body.data, 'pong');
  });
});

function encodeClientFrame(opcode, payload, fin = true, mask = Buffer.from([1, 2, 3, 4])) {
  if (!Buffer.isBuffer(payload)) payload = Buffer.from(payload);
  let header;
  if (payload.length < 126) {
    header = Buffer.from([(fin ? 0x80 : 0) | opcode, 0x80 | payload.length]);
  } else {
    header = Buffer.alloc(4);
    header[0] = (fin ? 0x80 : 0) | opcode;
    header[1] = 0x80 | 126;
    header.writeUInt16BE(payload.length, 2);
  }
  const masked = Buffer.alloc(payload.length);
  for (let i = 0; i < payload.length; i++) masked[i] = payload[i] ^ mask[i % 4];
  return Buffer.concat([header, mask, masked]);
}

function parseServerFrame(buffer) {
  if (buffer.length < 2) return null;
  const first = buffer[0];
  const second = buffer[1];
  assert.equal(second & 0x80, 0, 'server frame must not be masked');
  let length = second & 0x7f;
  let cursor = 2;
  if (length === 126) {
    if (buffer.length < 4) return null;
    length = buffer.readUInt16BE(2);
    cursor = 4;
  } else if (length === 127) {
    if (buffer.length < 10) return null;
    length = Number(buffer.readBigUInt64BE(2));
    cursor = 10;
  }
  if (buffer.length < cursor + length) return null;
  return {
    fin: (first & 0x80) !== 0,
    opcode: first & 0x0f,
    payload: buffer.subarray(cursor, cursor + length),
    bytes: cursor + length
  };
}

class RawWebSocketClient {
  constructor(socket, initial) {
    this.socket = socket;
    this.buffer = initial;
    this.waiters = [];
    this.closed = new Promise(resolve => socket.once('close', resolve));
    socket.on('data', chunk => {
      this.buffer = Buffer.concat([this.buffer, chunk]);
      this.drain();
    });
    socket.on('error', error => {
      while (this.waiters.length) this.waiters.shift().reject(error);
    });
    this.drain();
  }

  static async connect(port) {
    const socket = net.connect({ host: '127.0.0.1', port });
    const key = crypto.randomBytes(16).toString('base64');
    let input = Buffer.alloc(0);
    await new Promise((resolve, reject) => {
      const onData = chunk => {
        input = Buffer.concat([input, chunk]);
        const end = input.indexOf('\r\n\r\n');
        if (end < 0) return;
        socket.off('data', onData);
        const header = input.subarray(0, end).toString('ascii');
        assert.match(header, /^HTTP\/1\.1 101 /);
        input = input.subarray(end + 4);
        resolve();
      };
      socket.on('data', onData);
      socket.once('error', reject);
      socket.once('connect', () => {
        socket.write('GET /socket.io/?EIO=4&transport=websocket&guid=fixture-only HTTP/1.1\r\n' +
          'Host: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
          `Sec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`);
      });
    });
    return new RawWebSocketClient(socket, input);
  }

  drain() {
    while (this.waiters.length) {
      const frame = parseServerFrame(this.buffer);
      if (!frame) return;
      this.buffer = this.buffer.subarray(frame.bytes);
      this.waiters.shift().resolve(frame);
    }
  }

  readFrame() {
    const frame = parseServerFrame(this.buffer);
    if (frame) {
      this.buffer = this.buffer.subarray(frame.bytes);
      return Promise.resolve(frame);
    }
    return new Promise((resolve, reject) => {
      this.waiters.push({ resolve, reject });
      this.drain();
    });
  }

  async readText() {
    const frame = await this.readFrame();
    assert.equal(frame.fin, true);
    assert.equal(frame.opcode, 1);
    return frame.payload.toString('utf8');
  }

  sendText(text) {
    this.socket.write(encodeClientFrame(1, text));
  }

  sendFragmentedText(text, splitAt) {
    const bytes = Buffer.from(text);
    this.socket.write(encodeClientFrame(1, bytes.subarray(0, splitAt), false));
    this.socket.write(encodeClientFrame(0, bytes.subarray(splitAt), true));
  }

  async close() {
    if (this.socket.destroyed) return;
    this.socket.end(encodeClientFrame(8, Buffer.alloc(0)));
    await this.closed;
  }
}

test('Engine.IO websocket transcript supports open/connect, arbitrary ACK IDs, fragmentation, heartbeat, and pushes', async () => {
  const client = await RawWebSocketClient.connect(Number(new URL(origin).port));
  try {
    const open = await client.readText();
    assert.equal(open[0], '0');
    const openData = JSON.parse(open.slice(1));
    assert.equal(openData.pingInterval, 60000);
    assert.equal(openData.pingTimeout, 120000);
    assert.equal(openData.maxPayload, 1000000);

    client.sendFragmentedText('40', 1);
    const connected = await client.readText();
    assert.match(connected, /^40\{"sid":"socket-\d+"\}$/);

    const event = '42731["get-chats",{"withParticipants":true,"offset":0,"limit":1}]';
    client.sendFragmentedText(event, 17);
    const ack = await client.readText();
    assert.match(ack, /^43731\[/);
    const ackBody = JSON.parse(ack.slice('43731'.length));
    assert.equal(ackBody.length, 1);
    assertEnvelope(ackBody[0]);
    assert.equal(ackBody[0].data.length, 1);
    assertChat(ackBody[0].data[0]);

    fixture.sendEnginePing(openData.sid, 'probe');
    assert.equal(await client.readText(), '2probe');
    client.sendText('3probe');
    await waitFor(() => fixture.getPong(openData.sid) === 'probe');
    assert.equal(fixture.getPong(openData.sid), 'probe');

    const pushed = cloneForPush(fixture.data.messages[0]);
    fixture.broadcast('new-message', pushed);
    const pushPacket = await client.readText();
    assert.match(pushPacket, /^42\[/);
    const [eventName, payload] = JSON.parse(pushPacket.slice(2));
    assert.equal(eventName, 'new-message');
    assert.equal(payload.status, undefined, 'push payload must not be a response envelope');
    assertMessage(payload);

    const updated = cloneForPush(fixture.data.messages[1]);
    fixture.broadcast('updated-message', updated);
    const updatePacket = await client.readText();
    const [updateName, updatePayload] = JSON.parse(updatePacket.slice(2));
    assert.equal(updateName, 'updated-message');
    assert.equal(updatePayload.status, undefined);
    assertMessage(updatePayload);
  } finally {
    await client.close();
  }
});

function cloneForPush(value) {
  return JSON.parse(JSON.stringify(value));
}

test('Engine.IO polling transcript supports record separators, namespace connect, ACK, and pong', async () => {
  const openResponse = await fetch(`${origin}/socket.io/?EIO=4&transport=polling&guid=fixture-only`);
  assert.equal(openResponse.status, 200);
  const open = await openResponse.text();
  assert.equal(open[0], '0');
  const { sid } = JSON.parse(open.slice(1));

  const post = async body => fetch(
    `${origin}/socket.io/?EIO=4&transport=polling&sid=${encodeURIComponent(sid)}&guid=fixture-only`,
    { method: 'POST', headers: { 'Content-Type': 'text/plain;charset=UTF-8' }, body }
  );
  const poll = async () => (await fetch(
    `${origin}/socket.io/?EIO=4&transport=polling&sid=${encodeURIComponent(sid)}&guid=fixture-only`
  )).text();

  assert.equal(await (await post('40')).text(), 'ok');
  assert.match(await poll(), /^40\{"sid":"socket-\d+"\}$/);

  fixture.sendEnginePing(sid, 'polling-probe');
  const ackEvent = '4219["get-server-metadata",{}]';
  assert.equal(await (await post(`3polling-probe${RECORD_SEPARATOR}${ackEvent}`)).text(), 'ok');
  assert.equal(fixture.getPong(sid), 'polling-probe');
  const packets = (await poll()).split(RECORD_SEPARATOR);
  assert.equal(packets[0], '2polling-probe');
  assert.match(packets[1], /^4319\[/);
  const ack = JSON.parse(packets[1].slice('4319'.length))[0];
  assertEnvelope(ack);
  assert.equal(ack.data.server_version, '0.2.0');
});

test('Socket.IO packet parser rejects malformed packets without comma splitting', () => {
  const parsed = parseSocketEventPacket('421234["get-messages",{"text":"comma, slash / and ],[ stay JSON"}]');
  assert.equal(parsed.ackId, '1234');
  assert.equal(parsed.event, 'get-messages');
  assert.equal(parsed.data.text, 'comma, slash / and ],[ stay JSON');
  assert.throws(() => parseSocketEventPacket('42x["event",{}]'), /Malformed/);
  assert.throws(() => parseSocketEventPacket('42{}'), /array/i);
});

test('fixture transcript is structural and redacts credentials and dynamic identifiers', async () => {
  const local = createFixtureServer();
  const localOrigin = await local.start();
  try {
    await fetch(`${localOrigin}/api/v1/chat/${encodeURIComponent('private-chat-guid')}/message?` +
      'guid=fixture-only&after=0&limit=25');
    const transcript = local.getTranscript();
    assert.equal(transcript.length, 1);
    assert.deepEqual(transcript[0], {
      sequence: 1,
      kind: 'http',
      method: 'GET',
      path: '/api/v1/chat/:guid/message',
      queryKeys: ['after', 'limit']
    });
    const encoded = JSON.stringify(transcript);
    assert.ok(!encoded.includes('fixture-only'));
    assert.ok(!encoded.includes('private-chat-guid'));
  } finally {
    await local.stop();
  }
});
