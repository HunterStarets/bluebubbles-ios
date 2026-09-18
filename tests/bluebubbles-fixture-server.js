'use strict';

const crypto = require('node:crypto');
const http = require('node:http');

const MAX_BODY_BYTES = 1024 * 1024;
const MAX_WS_PAYLOAD_BYTES = 1024 * 1024;
const RECORD_SEPARATOR = '\x1e';

function clone(value) {
  return JSON.parse(JSON.stringify(value));
}

function success(data, message = 'Success', metadata) {
  const response = { status: 200, message, data };
  if (metadata !== undefined) response.metadata = metadata;
  return response;
}

function error(status, message, type, detail) {
  return { status, message, error: { type, message: detail } };
}

function fixtureData() {
  const handles = [
    {
      originalROWID: 101,
      address: '+15555550100',
      service: 'iMessage',
      country: 'US',
      uncanonicalizedId: '+1 (555) 555-0100'
    },
    {
      originalROWID: 102,
      address: 'fixture.member@example.invalid',
      service: 'iMessage',
      country: null,
      uncanonicalizedId: null
    },
    {
      originalROWID: 103,
      address: '+15555550101',
      service: 'SMS',
      country: 'US',
      uncanonicalizedId: null
    }
  ];

  const chatBase = [
    {
      originalROWID: 201,
      guid: 'iMessage;-;+15555550100',
      style: 45,
      chatIdentifier: '+15555550100',
      isArchived: false,
      displayName: '',
      participants: [handles[0]],
      groupId: null,
      properties: null,
      lastAddressedHandle: null
    },
    {
      originalROWID: 202,
      guid: 'iMessage;+;fixture-group-1',
      style: 43,
      chatIdentifier: 'fixture-group-1',
      isArchived: false,
      displayName: 'Fixture Group',
      participants: [handles[0], handles[1]],
      groupId: 'fixture-group-1',
      properties: null,
      lastAddressedHandle: null
    }
  ];

  const attachments = [
    {
      originalROWID: 401,
      guid: 'fixture-attachment-downloaded',
      uti: 'public.png',
      mimeType: 'image/png',
      transferName: 'fixture.png',
      totalBytes: 24,
      transferState: 5,
      isOutgoing: false,
      hideAttachment: false,
      isSticker: false,
      originalGuid: null,
      hasLivePhoto: false,
      width: 1,
      height: 1,
      metadata: { fixture: true }
    },
    {
      originalROWID: 402,
      guid: 'fixture-attachment-missing',
      uti: 'public.data',
      mimeType: 'application/octet-stream',
      transferName: 'missing.bin',
      totalBytes: 4096,
      transferState: 0,
      isOutgoing: false,
      hideAttachment: false,
      isSticker: false,
      originalGuid: null,
      hasLivePhoto: false,
      width: 0,
      height: 0,
      metadata: null
    }
  ];

  const messageDefaults = {
    attributedBody: null,
    otherHandle: 0,
    subject: '',
    error: 0,
    dateRead: null,
    dateDelivered: null,
    isDelivered: false,
    isArchived: false,
    itemType: 0,
    groupTitle: null,
    groupActionType: 0,
    balloonBundleId: null,
    associatedMessageGuid: null,
    associatedMessageType: null,
    expressiveSendStyleId: null,
    replyToGuid: null,
    threadOriginatorGuid: null,
    threadOriginatorPart: null,
    dateEdited: null,
    dateRetracted: null,
    partCount: 1,
    messageSummaryInfo: null,
    payloadData: null,
    hasPayloadData: false,
    hasDdResults: false,
    isAudioMessage: false,
    datePlayed: null,
    country: null,
    isDelayed: false,
    isAutoReply: false,
    isSystemMessage: false,
    isServiceMessage: false,
    isForward: false,
    isExpired: false,
    shareStatus: null,
    shareDirection: null
  };

  const minimalChat = chat => ({
    originalROWID: chat.originalROWID,
    guid: chat.guid,
    style: chat.style,
    chatIdentifier: chat.chatIdentifier,
    isArchived: chat.isArchived,
    displayName: chat.displayName,
    participants: clone(chat.participants)
  });

  const messages = [
    {
      ...messageDefaults,
      originalROWID: 301,
      guid: 'fixture-message-direct-incoming',
      text: 'Synthetic incoming fixture message',
      handle: handles[0],
      handleId: handles[0].originalROWID,
      attachments: [],
      dateCreated: 1700000000000,
      isFromMe: false,
      chats: [minimalChat(chatBase[0])]
    },
    {
      ...messageDefaults,
      originalROWID: 302,
      guid: 'fixture-message-direct-outgoing',
      text: 'Synthetic delivered fixture message',
      handle: null,
      handleId: 0,
      attachments: [],
      dateCreated: 1700000060000,
      dateDelivered: 1700000065000,
      dateRead: 1700000070000,
      isDelivered: true,
      isFromMe: true,
      chats: [minimalChat(chatBase[0])]
    },
    {
      ...messageDefaults,
      originalROWID: 303,
      guid: 'fixture-message-group-media',
      text: '',
      handle: handles[1],
      handleId: handles[1].originalROWID,
      attachments: [attachments[0]],
      dateCreated: 1700000120000,
      isFromMe: false,
      chats: [minimalChat(chatBase[1])]
    },
    {
      ...messageDefaults,
      originalROWID: 304,
      guid: 'fixture-message-group-missing-media',
      text: '',
      handle: handles[0],
      handleId: handles[0].originalROWID,
      attachments: [attachments[1]],
      dateCreated: 1700000180000,
      isFromMe: false,
      chats: [minimalChat(chatBase[1])]
    }
  ];

  const byChat = new Map(chatBase.map(chat => [
    chat.guid,
    messages.filter(message => message.chats.some(item => item.guid === chat.guid))
  ]));
  const chats = chatBase.map(chat => {
    const rows = byChat.get(chat.guid);
    return { ...chat, lastMessage: rows[rows.length - 1] ?? null };
  });
  const attachmentBytes = new Map([
    [attachments[0].guid, Buffer.from('fixture-attachment-bytes')]
  ]);

  return { handles, chats, messages, attachments, attachmentBytes, byChat };
}

function constantTimePasswordMatch(candidate, expected) {
  if (typeof candidate !== 'string') return false;
  const left = Buffer.from(candidate.trim());
  const right = Buffer.from(expected.trim());
  if (left.length !== right.length) return false;
  return crypto.timingSafeEqual(left, right);
}

function authenticate(url, password) {
  const candidate = url.searchParams.get('guid') ??
    url.searchParams.get('password') ?? url.searchParams.get('token');
  return constantTimePasswordMatch(candidate, password);
}

function parseInteger(value, fallback, minimum = 0, maximum = Number.MAX_SAFE_INTEGER) {
  if (value === null || value === undefined || value === '') return fallback;
  const parsed = Number(value);
  if (!Number.isSafeInteger(parsed) || parsed < minimum || parsed > maximum) return null;
  return parsed;
}

async function readBody(req, limit = MAX_BODY_BYTES) {
  const chunks = [];
  let total = 0;
  for await (const chunk of req) {
    total += chunk.length;
    if (total > limit) {
      const ex = new Error('Request body exceeds fixture limit');
      ex.code = 'BODY_TOO_LARGE';
      throw ex;
    }
    chunks.push(chunk);
  }
  return Buffer.concat(chunks);
}

async function readJson(req) {
  const body = await readBody(req);
  if (body.length === 0) return {};
  try {
    const value = JSON.parse(body.toString('utf8'));
    if (!value || Array.isArray(value) || typeof value !== 'object') {
      throw new Error('JSON body must be an object');
    }
    return value;
  } catch (ex) {
    const errorValue = new Error(ex.message);
    errorValue.code = 'MALFORMED_JSON';
    throw errorValue;
  }
}

function parseMultipart(contentType, body) {
  const match = /^multipart\/form-data\s*;\s*boundary=(?:"([^"]+)"|([^;\s]+))/i.exec(contentType ?? '');
  const boundary = match?.[1] ?? match?.[2];
  if (!boundary || boundary.length > 200) throw new Error('Missing or invalid multipart boundary');
  const raw = body.toString('latin1');
  const delimiter = `--${boundary}`;
  if (!raw.startsWith(`${delimiter}\r\n`) ||
      !(raw.endsWith(`${delimiter}--\r\n`) || raw.endsWith(`${delimiter}--`))) {
    throw new Error('Malformed multipart framing');
  }
  const fields = {};
  const files = {};
  const parts = raw.split(delimiter).slice(1, -1);
  for (const rawPart of parts) {
    if (!rawPart.startsWith('\r\n') || !rawPart.endsWith('\r\n')) {
      throw new Error('Malformed multipart part');
    }
    const part = rawPart.slice(2, -2);
    const headerEnd = part.indexOf('\r\n\r\n');
    if (headerEnd < 0) throw new Error('Multipart part is missing headers');
    const headers = part.slice(0, headerEnd);
    const content = part.slice(headerEnd + 4);
    const disposition = /^Content-Disposition:\s*form-data;([^\r\n]+)$/im.exec(headers);
    const name = /(?:^|;)\s*name="([^"]+)"/i.exec(disposition?.[1] ?? '')?.[1];
    const filename = /(?:^|;)\s*filename="([^"]*)"/i.exec(disposition?.[1] ?? '')?.[1];
    if (!name) throw new Error('Multipart part is missing a name');
    if (filename !== undefined) {
      files[name] = { filename, data: Buffer.from(content, 'latin1') };
    } else {
      fields[name] = Buffer.from(content, 'latin1').toString('utf8');
    }
  }
  return { fields, files };
}

function corsHeaders(extra = {}) {
  return {
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Methods': 'GET,POST,PUT,DELETE,OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type,Range',
    ...extra
  };
}

function sendJson(res, statusCode, body, extraHeaders = {}) {
  const encoded = Buffer.from(JSON.stringify(body));
  res.writeHead(statusCode, corsHeaders({
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': encoded.length,
    ...extraHeaders
  }));
  res.end(encoded);
}

function sendText(res, statusCode, body, extraHeaders = {}) {
  const encoded = Buffer.from(body);
  res.writeHead(statusCode, corsHeaders({
    'Content-Type': 'text/plain; charset=UTF-8',
    'Content-Length': encoded.length,
    ...extraHeaders
  }));
  res.end(encoded);
}

function parseRange(value, size) {
  if (!value) return null;
  const match = /^bytes=(\d*)-(\d*)$/.exec(value.trim());
  if (!match || (!match[1] && !match[2])) return false;
  let start;
  let end;
  if (!match[1]) {
    const suffix = Number(match[2]);
    if (!Number.isSafeInteger(suffix) || suffix <= 0) return false;
    start = Math.max(0, size - suffix);
    end = size - 1;
  } else {
    start = Number(match[1]);
    end = match[2] ? Number(match[2]) : size - 1;
    if (!Number.isSafeInteger(start) || !Number.isSafeInteger(end) || start > end || start >= size) return false;
    end = Math.min(end, size - 1);
  }
  return { start, end };
}

function sendBytes(req, res, data, mimeType) {
  const range = parseRange(req.headers.range, data.length);
  if (range === false) {
    res.writeHead(416, corsHeaders({ 'Content-Range': `bytes */${data.length}`, 'Content-Length': 0 }));
    res.end();
    return;
  }
  if (range) {
    const part = data.subarray(range.start, range.end + 1);
    res.writeHead(206, corsHeaders({
      'Content-Type': mimeType,
      'Content-Length': part.length,
      'Accept-Ranges': 'bytes',
      'Content-Range': `bytes ${range.start}-${range.end}/${data.length}`
    }));
    res.end(part);
    return;
  }
  res.writeHead(200, corsHeaders({
    'Content-Type': mimeType,
    'Content-Length': data.length,
    'Accept-Ranges': 'bytes'
  }));
  res.end(data);
}

function page(values, offset, limit) {
  return values.slice(offset, offset + limit);
}

function filterMessages(messages, options) {
  let output = messages.slice();
  if (options.chatGuid) {
    output = output.filter(message => message.chats.some(chat => chat.guid === options.chatGuid));
  }
  if (options.after !== null) output = output.filter(message => message.dateCreated > options.after);
  if (options.before !== null) output = output.filter(message => message.dateCreated < options.before);
  output.sort((a, b) => options.sort === 'ASC' ? a.dateCreated - b.dateCreated : b.dateCreated - a.dateCreated);
  return output;
}

function serverMetadata() {
  return {
    computer_id: 'bluebubbles-ios-fixture',
    os_version: '9.3.5',
    server_version: '0.2.0',
    private_api: false,
    helper_connected: false,
    proxy_service: 'iOS Bridge Fixture',
    detected_icloud: '',
    detected_imessage: '',
    macos_time_sync: null,
    local_ipv4s: ['127.0.0.1'],
    local_ipv6s: ['::1'],
    ios_capabilities: {
      text: true,
      attachments: true,
      groups: true,
      reactions: false,
      effects: false,
      replies: false,
      edit: false,
      unsend: false,
      scheduling: false
    }
  };
}

function sanitizePath(pathname) {
  const patterns = [
    [/^\/api\/v1\/chat\/[^/]+\/message$/, '/api/v1/chat/:guid/message'],
    [/^\/api\/v1\/chat\/[^/]+$/, '/api/v1/chat/:guid'],
    [/^\/api\/v1\/attachment\/[^/]+\/download$/, '/api/v1/attachment/:guid/download'],
    [/^\/api\/v1\/attachment\/[^/]+$/, '/api/v1/attachment/:guid'],
    [/^\/api\/v1\/message\/[^/]+$/, '/api/v1/message/:guid'],
    [/^\/api\/v1\/handle\/[^/]+$/, '/api/v1/handle/:guid']
  ];
  for (const [pattern, replacement] of patterns) {
    if (pattern.test(pathname)) return replacement;
  }
  return pathname;
}

function encodeServerFrame(opcode, payload = Buffer.alloc(0), fin = true) {
  if (!Buffer.isBuffer(payload)) payload = Buffer.from(payload);
  if (payload.length > MAX_WS_PAYLOAD_BYTES) throw new Error('WebSocket payload exceeds fixture limit');
  let header;
  if (payload.length < 126) {
    header = Buffer.from([(fin ? 0x80 : 0) | opcode, payload.length]);
  } else if (payload.length <= 0xffff) {
    header = Buffer.alloc(4);
    header[0] = (fin ? 0x80 : 0) | opcode;
    header[1] = 126;
    header.writeUInt16BE(payload.length, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = (fin ? 0x80 : 0) | opcode;
    header[1] = 127;
    header.writeBigUInt64BE(BigInt(payload.length), 2);
  }
  return Buffer.concat([header, payload]);
}

class WebSocketPeer {
  constructor(socket, onText, onClose) {
    this.socket = socket;
    this.onText = onText;
    this.onClose = onClose;
    this.buffer = Buffer.alloc(0);
    this.fragmentOpcode = null;
    this.fragments = [];
    this.fragmentBytes = 0;
    this.closed = false;
    socket.on('data', chunk => this.consume(chunk));
    socket.on('close', () => this.finishClose());
    socket.on('error', () => this.finishClose());
  }

  sendText(text) {
    if (!this.closed) this.socket.write(encodeServerFrame(0x1, Buffer.from(text)));
  }

  sendPing(payload = '') {
    if (!this.closed) this.socket.write(encodeServerFrame(0x9, Buffer.from(payload)));
  }

  close() {
    if (this.closed) return;
    this.closed = true;
    this.socket.end(encodeServerFrame(0x8));
    this.onClose?.();
  }

  finishClose() {
    if (this.closed) return;
    this.closed = true;
    this.onClose?.();
  }

  fail() {
    this.socket.destroy();
    this.finishClose();
  }

  consume(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    while (this.buffer.length >= 2) {
      const first = this.buffer[0];
      const second = this.buffer[1];
      const fin = (first & 0x80) !== 0;
      const opcode = first & 0x0f;
      const masked = (second & 0x80) !== 0;
      let payloadLength = second & 0x7f;
      let cursor = 2;
      if ((first & 0x70) !== 0 || !masked || ![0, 1, 2, 8, 9, 10].includes(opcode)) return this.fail();
      if (payloadLength === 126) {
        if (this.buffer.length < 4) return;
        payloadLength = this.buffer.readUInt16BE(2);
        if (payloadLength < 126) return this.fail();
        cursor = 4;
      } else if (payloadLength === 127) {
        if (this.buffer.length < 10 || (this.buffer[2] & 0x80) !== 0) return this.fail();
        const length64 = this.buffer.readBigUInt64BE(2);
        if (length64 <= 0xffffn || length64 > BigInt(MAX_WS_PAYLOAD_BYTES)) return this.fail();
        payloadLength = Number(length64);
        cursor = 10;
      }
      if (payloadLength > MAX_WS_PAYLOAD_BYTES) return this.fail();
      const isControl = opcode >= 8;
      if (isControl && (!fin || payloadLength > 125)) return this.fail();
      const frameLength = cursor + 4 + payloadLength;
      if (this.buffer.length < frameLength) return;
      const mask = this.buffer.subarray(cursor, cursor + 4);
      cursor += 4;
      const payload = Buffer.alloc(payloadLength);
      for (let i = 0; i < payloadLength; i++) payload[i] = this.buffer[cursor + i] ^ mask[i % 4];
      this.buffer = this.buffer.subarray(frameLength);

      if (opcode === 8) return this.close();
      if (opcode === 9) {
        this.socket.write(encodeServerFrame(10, payload));
        continue;
      }
      if (opcode === 10) continue;
      if (opcode === 0) {
        if (this.fragmentOpcode === null) return this.fail();
        this.fragments.push(payload);
        this.fragmentBytes += payload.length;
        if (this.fragmentBytes > MAX_WS_PAYLOAD_BYTES) return this.fail();
        if (fin) {
          const completeOpcode = this.fragmentOpcode;
          const complete = Buffer.concat(this.fragments, this.fragmentBytes);
          this.fragmentOpcode = null;
          this.fragments = [];
          this.fragmentBytes = 0;
          if (completeOpcode === 1) this.onText(complete.toString('utf8'));
        }
        continue;
      }
      if (this.fragmentOpcode !== null) return this.fail();
      if (!fin) {
        this.fragmentOpcode = opcode;
        this.fragments = [payload];
        this.fragmentBytes = payload.length;
      } else if (opcode === 1) {
        this.onText(payload.toString('utf8'));
      }
    }
  }
}

function parseSocketEventPacket(packet) {
  if (!packet.startsWith('42')) throw new Error('Not a Socket.IO event packet');
  let cursor = 2;
  while (cursor < packet.length && /[0-9]/.test(packet[cursor])) cursor++;
  const ackId = cursor > 2 ? packet.slice(2, cursor) : null;
  let payload;
  try {
    payload = JSON.parse(packet.slice(cursor));
  } catch {
    throw new Error('Malformed Socket.IO event JSON');
  }
  if (!Array.isArray(payload) || typeof payload[0] !== 'string') {
    throw new Error('Socket.IO event payload must be an array beginning with an event name');
  }
  return { ackId, event: payload[0], data: payload[1] ?? {} };
}

function createFixtureServer(options = {}) {
  const password = options.password ?? 'fixture-only';
  const data = fixtureData();
  const transcript = [];
  const pollingSessions = new Map();
  const webSocketSessions = new Map();
  const pongs = new Map();
  let sidCounter = 0;

  const record = entry => {
    const output = { sequence: transcript.length + 1, ...entry };
    transcript.push(output);
    if (typeof options.onRecord === 'function') options.onRecord(clone(output));
    if (options.logRequests) process.stderr.write(`${JSON.stringify(output)}\n`);
    return output;
  };

  const nextSid = prefix => `${prefix}-${++sidCounter}`;
  const openPacket = sid => `0${JSON.stringify({
    sid,
    upgrades: [],
    pingInterval: 60000,
    pingTimeout: 120000,
    maxPayload: 1000000
  })}`;

  const chatPage = (offset = 0, limit = 100) => {
    const values = page(data.chats, offset, limit).map(clone);
    return success(values, 'Success', { offset, limit, total: data.chats.length, count: values.length });
  };

  const messagePage = ({ chatGuid = null, after = null, before = null, sort = 'DESC', offset = 0, limit = 100 }) => {
    const filtered = filterMessages(data.messages, { chatGuid, after, before, sort });
    const values = page(filtered, offset, limit).map(clone);
    return success(values, 'Successfully fetched messages!', {
      offset, limit, total: filtered.length, count: values.length
    });
  };

  // POST /chat/new and start-chat share the iOS 9 rules: addresses (or
  // participants: string | string[]), a mandatory first message, service
  // iMessage/SMS, optional tempGuid; effects/subjects/attributed bodies are
  // refused. The reply is a ChatResponse with participants and the sent
  // message (no chats expansion inside it), as the reference serializes it.
  let createdCounter = 0;
  const createChat = (body, addressesKey) => {
    if (!body || typeof body !== 'object' || Array.isArray(body)) {
      return error(400, 'Bad Request', 'Validation Error', 'Malformed JSON body');
    }
    if (body.effectId != null) return error(400, 'Bad Request', 'Validation Error', 'Message effects are not supported on iOS 9');
    if (body.subject != null) return error(400, 'Bad Request', 'Validation Error', 'Message subjects are not supported on iOS 9');
    if (body.attributedBody != null) return error(400, 'Bad Request', 'Validation Error', 'Attributed bodies are not supported on iOS 9');
    let addresses = body[addressesKey];
    if (typeof addresses === 'string') addresses = [addresses];
    if (addresses == null) return error(400, 'Bad Request', 'Validation Error', 'Missing addresses');
    if (!Array.isArray(addresses)) return error(400, 'Bad Request', 'Validation Error', 'Addresses must be an array');
    if (addresses.length === 0) return error(400, 'Bad Request', 'Validation Error', 'Missing addresses');
    if (addresses.length > 8) return error(400, 'Bad Request', 'Validation Error', 'Too many addresses');
    if (!addresses.every(item => typeof item === 'string')) return error(400, 'Bad Request', 'Validation Error', 'Addresses must be strings');
    if (addresses.some(item => item.length === 0)) return error(400, 'Bad Request', 'Validation Error', 'Empty address');
    if (body.message == null) return error(400, 'Bad Request', 'Validation Error', 'A message is required to create a chat on iOS 9');
    if (typeof body.message !== 'string') return error(400, 'Bad Request', 'Validation Error', 'Missing or oversized message');
    if (body.message.length === 0) return error(400, 'Bad Request', 'Validation Error', 'Empty message');
    let service = 'iMessage';
    if (body.service != null) {
      if (typeof body.service !== 'string') return error(400, 'Bad Request', 'Validation Error', 'Invalid service');
      if (body.service.toLowerCase() === 'sms') service = 'SMS';
      else if (body.service.toLowerCase() !== 'imessage') return error(400, 'Bad Request', 'Validation Error', 'Invalid service');
    }
    if (body.tempGuid != null && typeof body.tempGuid !== 'string') {
      return error(400, 'Bad Request', 'Validation Error', 'Invalid tempGuid');
    }
    createdCounter += 1;
    const group = addresses.length > 1;
    const identifier = group ? `fixture-created-group-${createdCounter}` : addresses[0];
    const participants = addresses.map((address, index) => ({
      originalROWID: 900 + createdCounter * 10 + index,
      address,
      service,
      country: null,
      uncanonicalizedId: null
    }));
    const chat = {
      originalROWID: 800 + createdCounter,
      guid: `${service};${group ? '+' : '-'};${identifier}`,
      style: group ? 43 : 45,
      chatIdentifier: identifier,
      isArchived: false,
      displayName: '',
      groupId: null,
      properties: null,
      lastAddressedHandle: null,
      participants
    };
    const message = {
      originalROWID: 700 + createdCounter,
      guid: `fixture-created-message-${createdCounter}`,
      text: body.message,
      handle: null,
      handleId: 0,
      otherHandle: 0,
      attachments: [],
      subject: null,
      error: 0,
      dateCreated: Date.now(),
      dateRead: null,
      dateDelivered: null,
      isDelivered: true,
      isFromMe: true,
      isArchived: false,
      itemType: 0,
      groupTitle: null,
      groupActionType: 0,
      hasAttachments: false,
      balloonBundleId: null,
      associatedMessageGuid: null,
      associatedMessageType: null,
      expressiveSendStyleId: null,
      replyToGuid: null,
      threadOriginatorGuid: null,
      dateEdited: null,
      dateRetracted: null,
      partCount: 1,
      messageSummaryInfo: null,
      payloadData: null,
      hasPayloadData: false
    };
    if (typeof body.tempGuid === 'string' && body.tempGuid.length) message.tempGuid = body.tempGuid;
    return success({ ...chat, messages: [message] }, 'Successfully created chat!');
  };

  const socketResponse = (event, args) => {
    switch (event) {
      case 'start-chat':
        return createChat(args ?? null, 'participants');
      case 'get-server-metadata':
        return success(serverMetadata(), 'Successfully fetched metadata');
      case 'get-chats':
        return chatPage(parseInteger(args.offset, 0) ?? 0, parseInteger(args.limit, 100, 1, 1000) ?? 100);
      case 'get-chat': {
        const chat = data.chats.find(item => item.guid === args.chatGuid || item.guid === args.identifier);
        return chat ? success(clone(chat)) : error(404, 'Not Found', 'Database Error', 'Chat does not exist!');
      }
      case 'get-chat-messages':
        return messagePage({
          chatGuid: args.identifier ?? args.chatGuid,
          after: parseInteger(args.after, null),
          before: parseInteger(args.before, null),
          sort: args.sort === 'ASC' ? 'ASC' : 'DESC',
          offset: parseInteger(args.offset, 0) ?? 0,
          limit: parseInteger(args.limit, 100, 1, 1000) ?? 100
        });
      case 'get-messages':
        return messagePage({
          chatGuid: args.chatGuid ?? null,
          after: parseInteger(args.after, null),
          before: parseInteger(args.before, null),
          sort: args.sort === 'ASC' ? 'ASC' : 'DESC',
          offset: parseInteger(args.offset, 0) ?? 0,
          limit: parseInteger(args.limit, 100, 1, 1000) ?? 100
        });
      case 'get-attachment': {
        const attachment = data.attachments.find(item => item.guid === (args.identifier ?? args.guid));
        return attachment ? success(clone(attachment)) :
          error(404, 'Not Found', 'Database Error', 'Attachment does not exist!');
      }
      default:
        return error(400, 'Bad Request', 'Validation Error', `Unsupported fixture event: ${event}`);
    }
  };

  const handleEnginePacket = (session, packet) => {
    if (packet === '40') {
      record({ kind: 'engine-packet', transport: session.transport, packet: 'namespace-connect' });
      session.namespaceConnected = true;
      session.send(`40${JSON.stringify({ sid: session.socketSid })}`);
      return;
    }
    if (packet === '41') {
      record({ kind: 'engine-packet', transport: session.transport, packet: 'namespace-disconnect' });
      session.close?.();
      return;
    }
    if (packet.startsWith('3')) {
      record({ kind: 'engine-packet', transport: session.transport, packet: 'pong' });
      pongs.set(session.sid, packet.slice(1));
      return;
    }
    if (packet.startsWith('42')) {
      let parsed;
      try {
        parsed = parseSocketEventPacket(packet);
      } catch {
        session.close?.();
        return;
      }
      record({
        kind: 'socket-event',
        transport: session.transport,
        event: parsed.event,
        hasAck: parsed.ackId !== null,
        dataKeys: parsed.data && typeof parsed.data === 'object' && !Array.isArray(parsed.data) ?
          Object.keys(parsed.data).sort() : []
      });
      const response = socketResponse(parsed.event, parsed.data);
      if (parsed.ackId !== null) {
        session.send(`43${parsed.ackId}${JSON.stringify([response])}`);
      } else {
        session.send(`42${JSON.stringify([`${parsed.event}-response`, response])}`);
      }
    }
  };

  const handlePolling = async (req, res, url) => {
    const transport = url.searchParams.get('transport');
    if (transport !== 'polling' || url.searchParams.get('EIO') !== '4') {
      return sendJson(res, 400, error(400, 'Bad Request', 'Validation Error', 'Engine.IO v4 polling required'));
    }
    const sid = url.searchParams.get('sid');
    if (!sid) {
      if (req.method !== 'GET') {
        return sendJson(res, 400, error(400, 'Bad Request', 'Validation Error', 'Initial polling request must be GET'));
      }
      const engineSid = nextSid('engine');
      const session = {
        sid: engineSid,
        socketSid: nextSid('socket'),
        transport: 'polling',
        namespaceConnected: false,
        queue: [],
        send(packet) { this.queue.push(packet); }
      };
      pollingSessions.set(engineSid, session);
      return sendText(res, 200, openPacket(engineSid));
    }
    const session = pollingSessions.get(sid);
    if (!session) return sendJson(res, 400, error(400, 'Bad Request', 'Validation Error', 'Unknown Engine.IO sid'));
    if (req.method === 'POST') {
      const body = (await readBody(req)).toString('utf8');
      for (const packet of body.split(RECORD_SEPARATOR)) {
        if (packet) handleEnginePacket(session, packet);
      }
      return sendText(res, 200, 'ok');
    }
    if (req.method === 'GET') {
      const packets = session.queue.splice(0);
      return sendText(res, 200, packets.length ? packets.join(RECORD_SEPARATOR) : '2');
    }
    return sendJson(res, 405, error(400, 'Bad Request', 'Validation Error', 'Unsupported polling method'));
  };

  const handleRest = async (req, res, url, requestRecord) => {
    if (req.method === 'OPTIONS') {
      res.writeHead(204, corsHeaders({ 'Content-Length': 0 }));
      res.end();
      return;
    }
    if (!authenticate(url, password)) {
      return sendJson(res, 401, error(401, 'You are not authorized to access this resource',
        'Authentication Error', 'Unauthorized'));
    }
    const delay = parseInteger(url.searchParams.get('delay'), 0, 0, 100);
    if (delay) await new Promise(resolve => setTimeout(resolve, delay));
    const path = url.pathname;

    if (req.method === 'GET' && path === '/api/v1/ping') {
      return sendJson(res, 200, success('pong', 'Ping received!'));
    }
    if (req.method === 'GET' && path === '/api/v1/server/info') {
      return sendJson(res, 200, success(serverMetadata()));
    }
    if ((req.method === 'GET' && path === '/api/v1/contact') ||
        (req.method === 'POST' && path === '/api/v1/contact/query')) {
      if (req.method === 'POST') await readJson(req);
      return sendJson(res, 200, success([
        { id: 'ab-1', displayName: 'Fixture Person', firstName: 'Fixture', lastName: 'Person',
          phoneNumbers: [{ address: '+15555550100', label: 'mobile' }],
          emails: [{ address: 'fixture@example.invalid', label: 'home' }], avatar: '/9j/4AAQSkZJRg==' }
      ]));
    }
    if (req.method === 'GET' && path === '/api/v1/fcm/client') {
      return sendJson(res, 404, error(404, 'Not Found', 'Capability Error',
        'FCM is not supported by the iOS 9 bridge'));
    }
    if (req.method === 'GET' && path === '/api/v1/chat/count') {
      return sendJson(res, 200, success({ total: data.chats.length, breakdown: { iMessage: 2 } }));
    }
    if (req.method === 'POST' && path === '/api/v1/chat/query') {
      const body = await readJson(req);
      requestRecord.bodyKeys = Object.keys(body).sort();
      const offset = parseInteger(body.offset, 0);
      const limit = parseInteger(body.limit, 100, 1, 1000);
      if (offset === null || limit === null) {
        return sendJson(res, 400, error(400, 'Bad Request', 'Validation Error', 'Invalid pagination'));
      }
      return sendJson(res, 200, chatPage(offset, limit));
    }
    if (path === '/api/v1/chat/new') {
      if (req.method !== 'POST') {
        return sendJson(res, 405, error(405, 'Method Not Allowed', 'Validation Error', 'Method not allowed'));
      }
      const body = await readJson(req);
      requestRecord.bodyKeys = Object.keys(body).sort();
      const reply = createChat(body, 'addresses');
      return sendJson(res, reply.status, reply);
    }
    const chatMatch = /^\/api\/v1\/chat\/([^/]+)$/.exec(path);
    if (req.method === 'GET' && chatMatch) {
      const chat = data.chats.find(item => item.guid === decodeURIComponent(chatMatch[1]));
      if (!chat) {
        return sendJson(res, 404, error(404, 'The requested resource was not found',
          'Database Error', 'Chat does not exist!'));
      }
      return sendJson(res, 200, success(clone(chat)));
    }
    const historyMatch = /^\/api\/v1\/chat\/([^/]+)\/message$/.exec(path);
    if (req.method === 'GET' && historyMatch) {
      const chatGuid = decodeURIComponent(historyMatch[1]);
      if (!data.chats.some(chat => chat.guid === chatGuid)) {
        return sendJson(res, 404, error(404, 'The requested resource was not found',
          'Database Error', 'Chat does not exist!'));
      }
      const after = parseInteger(url.searchParams.get('after'), null);
      const before = parseInteger(url.searchParams.get('before'), null);
      const offset = parseInteger(url.searchParams.get('offset'), 0);
      const limit = parseInteger(url.searchParams.get('limit'), 100, 1, 1000);
      if (after === null && url.searchParams.has('after') || before === null && url.searchParams.has('before') ||
          offset === null || limit === null) {
        return sendJson(res, 400, error(400, 'Bad Request', 'Validation Error', 'Invalid message query'));
      }
      return sendJson(res, 200, messagePage({
        chatGuid, after, before, offset, limit,
        sort: url.searchParams.get('sort') === 'ASC' ? 'ASC' : 'DESC'
      }));
    }
    if (req.method === 'GET' && (path === '/api/v1/message/count' || path === '/api/v1/message/count/updated')) {
      const after = parseInteger(url.searchParams.get('after'), null);
      const before = parseInteger(url.searchParams.get('before'), null);
      const filtered = filterMessages(data.messages, { chatGuid: null, after, before, sort: 'DESC' });
      return sendJson(res, 200, success({ total: filtered.length }));
    }
    if (req.method === 'POST' && path === '/api/v1/message/query') {
      const body = await readJson(req);
      requestRecord.bodyKeys = Object.keys(body).sort();
      const offset = parseInteger(body.offset, 0);
      const limit = parseInteger(body.limit, 100, 1, 1000);
      const after = parseInteger(body.after, null);
      const before = parseInteger(body.before, null);
      if (offset === null || limit === null || (body.after != null && after === null) ||
          (body.before != null && before === null)) {
        return sendJson(res, 400, error(400, 'Bad Request', 'Validation Error', 'Invalid message query'));
      }
      return sendJson(res, 200, messagePage({
        chatGuid: typeof body.chatGuid === 'string' ? body.chatGuid : null,
        after,
        before,
        offset,
        limit,
        sort: body.sort === 'ASC' ? 'ASC' : 'DESC'
      }));
    }
    const attachmentDownloadMatch = /^\/api\/v1\/attachment\/([^/]+)\/download$/.exec(path);
    if (req.method === 'GET' && attachmentDownloadMatch) {
      const guid = decodeURIComponent(attachmentDownloadMatch[1]);
      const attachment = data.attachments.find(item => item.guid === guid);
      const bytes = data.attachmentBytes.get(guid);
      if (!attachment || !bytes) {
        return sendJson(res, 404, error(404, 'The requested resource was not found',
          'Database Error', 'Attachment is not downloaded on the server'));
      }
      return sendBytes(req, res, bytes, attachment.mimeType);
    }
    const attachmentMatch = /^\/api\/v1\/attachment\/([^/]+)$/.exec(path);
    if (req.method === 'GET' && attachmentMatch) {
      const guid = decodeURIComponent(attachmentMatch[1]);
      const attachment = data.attachments.find(item => item.guid === guid);
      return attachment ? sendJson(res, 200, success(clone(attachment))) :
        sendJson(res, 404, error(404, 'The requested resource was not found',
          'Database Error', 'Attachment does not exist!'));
    }
    // POST /message/attachment: the stock client's multipart send (file
    // part "attachment" plus chatGuid/tempGuid/name/method). Mirrors the
    // iOS 9 router: JSON bodies and missing parts are 400, effects /
    // subjects / replies are refused, the reply is a MessageResponse with
    // tempGuid and one attachment record.
    if (path === '/api/v1/message/attachment') {
      if (req.method !== 'POST') {
        return sendJson(res, 405, error(405, 'Method Not Allowed', 'Validation Error', 'Method not allowed'));
      }
      const body = await readBody(req);
      let multipart;
      try {
        multipart = parseMultipart(req.headers['content-type'], body);
      } catch (ex) {
        return sendJson(res, 400, error(400, 'Bad Request', 'Validation Error',
          /boundary/i.test(ex.message) ? 'Expected a multipart/form-data body' : ex.message));
      }
      requestRecord.bodyKeys = Object.keys(multipart.fields).sort();
      const file = multipart.files.attachment;
      const { chatGuid, tempGuid, name, effectId, subject, selectedMessageGuid } = multipart.fields;
      if (effectId) return sendJson(res, 400, error(400, 'Bad Request', 'Validation Error', 'Message effects are not supported on iOS 9'));
      if (subject) return sendJson(res, 400, error(400, 'Bad Request', 'Validation Error', 'Message subjects are not supported on iOS 9'));
      if (selectedMessageGuid) return sendJson(res, 400, error(400, 'Bad Request', 'Validation Error', 'Replies are not supported on iOS 9'));
      if (!file) return sendJson(res, 400, error(400, 'Bad Request', 'Validation Error', 'Multipart field attachment is required'));
      if (!chatGuid) return sendJson(res, 400, error(400, 'Bad Request', 'Validation Error', 'Missing chatGuid'));
      if (!tempGuid) return sendJson(res, 400, error(400, 'Bad Request', 'Validation Error', 'Missing tempGuid'));
      if (file.data.length === 0) return sendJson(res, 400, error(400, 'Bad Request', 'Validation Error', 'Attachment is empty'));
      if (!data.chats.some(chat => chat.guid === chatGuid)) {
        return sendJson(res, 404, error(404, 'Not Found', 'Database Error', 'Chat does not exist!'));
      }
      createdCounter += 1;
      const chat = data.chats.find(item => item.guid === chatGuid);
      const attachment = {
        originalROWID: 600 + createdCounter,
        guid: `fixture-sent-attachment-${createdCounter}`,
        uti: 'public.data',
        mimeType: 'application/octet-stream',
        transferName: name || file.filename || 'attachment',
        totalBytes: file.data.length,
        transferState: 5,
        isOutgoing: true,
        hideAttachment: false,
        isSticker: false,
        originalGuid: null,
        hasLivePhoto: false,
        width: 0,
        height: 0,
        metadata: null
      };
      const message = {
        originalROWID: 500 + createdCounter,
        guid: `fixture-sent-message-${createdCounter}`,
        tempGuid,
        text: '',
        handle: null,
        handleId: 0,
        otherHandle: 0,
        chats: [clone(chat)],
        attachments: [attachment],
        subject: null,
        error: 0,
        dateCreated: Date.now(),
        dateRead: null,
        dateDelivered: null,
        isDelivered: true,
        isFromMe: true,
        isArchived: false,
        itemType: 0,
        groupTitle: null,
        groupActionType: 0,
        hasAttachments: true,
        balloonBundleId: null,
        associatedMessageGuid: null,
        associatedMessageType: null,
        expressiveSendStyleId: null,
        replyToGuid: null,
        threadOriginatorGuid: null,
        dateEdited: null,
        dateRetracted: null,
        partCount: 1,
        messageSummaryInfo: null,
        payloadData: null,
        hasPayloadData: false
      };
      return sendJson(res, 200, success(message, 'Attachment sent!'));
    }
    if (req.method === 'POST' && path === '/api/v1/attachment/upload') {
      const body = await readBody(req);
      let multipart;
      try {
        multipart = parseMultipart(req.headers['content-type'], body);
      } catch (ex) {
        return sendJson(res, 400, error(400, 'Bad Request', 'Validation Error', ex.message));
      }
      const file = multipart.files.attachment;
      if (!file || file.data.length === 0) {
        return sendJson(res, 400, error(400, 'Bad Request', 'Validation Error',
          'Multipart field attachment is required'));
      }
      return sendJson(res, 200, success({ path: `fixture-upload/${file.filename || 'attachment'}` }));
    }
    return sendJson(res, 404, error(404, 'The requested resource was not found',
      'Database Error', 'Fixture route does not exist'));
  };

  const server = http.createServer(async (req, res) => {
    const url = new URL(req.url, 'http://127.0.0.1');
    const requestRecord = record({
      kind: url.pathname === '/socket.io/' ? 'engine-http' : 'http',
      method: req.method,
      path: sanitizePath(url.pathname),
      queryKeys: Array.from(url.searchParams.keys())
        .filter(key => !['guid', 'password', 'token', 'sid'].includes(key))
        .sort()
    });
    try {
      if (url.pathname === '/socket.io/') {
        if (!authenticate(url, password)) {
          return sendJson(res, 401, error(401, 'You are not authorized to access this resource',
            'Authentication Error', 'Unauthorized'));
        }
        return await handlePolling(req, res, url);
      }
      return await handleRest(req, res, url, requestRecord);
    } catch (ex) {
      if (ex.code === 'MALFORMED_JSON') {
        return sendJson(res, 400, error(400, 'Bad Request', 'Validation Error', 'Malformed JSON body'));
      }
      if (ex.code === 'BODY_TOO_LARGE') {
        return sendJson(res, 413, error(400, 'Bad Request', 'Validation Error', ex.message));
      }
      return sendJson(res, 500, error(500, 'Server Error', 'Server Error', ex.message));
    }
  });

  server.on('upgrade', (req, socket, head) => {
    const url = new URL(req.url, 'http://127.0.0.1');
    record({
      kind: 'engine-upgrade',
      method: req.method,
      path: sanitizePath(url.pathname),
      transport: url.searchParams.get('transport') ?? '',
      eio: url.searchParams.get('EIO') ?? ''
    });
    const reject = (status, reason) => {
      const body = Buffer.from(JSON.stringify(error(status, reason, 'Authentication Error', reason)));
      socket.end(`HTTP/1.1 ${status} ${reason}\r\nContent-Type: application/json\r\n` +
        `Content-Length: ${body.length}\r\nConnection: close\r\n\r\n${body}`);
    };
    if (url.pathname !== '/socket.io/' || url.searchParams.get('EIO') !== '4' ||
        url.searchParams.get('transport') !== 'websocket') return reject(400, 'Bad Request');
    if (!authenticate(url, password)) return reject(401, 'Unauthorized');
    const key = req.headers['sec-websocket-key'];
    if (req.headers.upgrade?.toLowerCase() !== 'websocket' || !key ||
        req.headers['sec-websocket-version'] !== '13') return reject(400, 'Bad Request');
    const accept = crypto.createHash('sha1')
      .update(`${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11`)
      .digest('base64');
    socket.write('HTTP/1.1 101 Switching Protocols\r\n' +
      'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
    const sid = nextSid('engine');
    const session = {
      sid,
      socketSid: nextSid('socket'),
      transport: 'websocket',
      namespaceConnected: false,
      peer: null,
      send(packet) { this.peer.sendText(packet); },
      close() { this.peer.close(); }
    };
    const peer = new WebSocketPeer(socket,
      packet => handleEnginePacket(session, packet),
      () => webSocketSessions.delete(sid));
    session.peer = peer;
    webSocketSessions.set(sid, session);
    peer.sendText(openPacket(sid));
    if (head.length) peer.consume(head);
  });

  return {
    password,
    data,
    server,
    async start(port = 0) {
      await new Promise((resolve, reject) => {
        server.once('error', reject);
        server.listen(port, '127.0.0.1', resolve);
      });
      return `http://127.0.0.1:${server.address().port}`;
    },
    async stop() {
      for (const session of webSocketSessions.values()) session.peer.close();
      await new Promise(resolve => server.close(resolve));
    },
    sendEnginePing(sid, payload = '') {
      const ws = webSocketSessions.get(sid);
      const polling = pollingSessions.get(sid);
      if (ws) ws.send(`2${payload}`);
      else if (polling) polling.send(`2${payload}`);
      else throw new Error('Unknown fixture session');
    },
    getPong(sid) {
      return pongs.get(sid);
    },
    getTranscript() {
      return clone(transcript);
    },
    broadcast(event, payload) {
      const packet = `42${JSON.stringify([event, payload])}`;
      for (const session of webSocketSessions.values()) {
        if (session.namespaceConnected) session.send(packet);
      }
      for (const session of pollingSessions.values()) {
        if (session.namespaceConnected) session.send(packet);
      }
    }
  };
}

if (require.main === module) {
  const fixture = createFixtureServer({
    password: process.env.BLUEBUBBLES_FIXTURE_PASSWORD ?? 'fixture-only',
    logRequests: process.env.BLUEBUBBLES_FIXTURE_LOG_REQUESTS === '1'
  });
  fixture.start(Number(process.env.PORT ?? 0)).then(origin => {
    process.stdout.write(`${origin}\n`);
  });
  const shutdown = () => fixture.stop().finally(() => process.exit(0));
  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
}

module.exports = {
  RECORD_SEPARATOR,
  createFixtureServer,
  encodeServerFrame,
  error,
  fixtureData,
  parseMultipart,
  parseSocketEventPacket,
  sanitizePath,
  serverMetadata,
  success
};
