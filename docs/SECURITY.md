# Security

## Threat model

This server exposes your iMessage and SMS history and lets whoever holds
the password send messages as you. Treat the password like the password to
your messages, because it is.

- **LAN only by default.** The listener binds every interface on the
  device, but nothing forwards it beyond your network. That is the same
  posture as the stock BlueBubbles server, which is plain HTTP locally and
  relies on a tunnel (Cloudflare/ngrok) for remote access.
- **One credential.** Every request carries the password as a query
  parameter (`guid=`, `password=`, or `token=`); it is compared in constant
  time. There are no accounts, sessions, or tokens beyond that.
- **Plain HTTP on the LAN** means anyone who can sniff your LAN can read the
  password and the traffic. On a home network that is the same exposure as
  the stock server. If that is not acceptable, enable TLS ([TLS.md](TLS.md))
  or put a tunnel in front.
- **Never expose port 1234 directly to the internet.** Use a tunnel with
  its own TLS (and, ideally, an access policy) on another machine.

## What the server does with data

- **Diagnostics are count-only.** `/tmp/bbdiag-sb.log` and
  `/tmp/bbdiag-sms.log` hold fixed event names with a build, pid, and time.
  No message text, address, name, guid, or credential can enter them by
  construction: the trace API takes an enum, not a string.
- **Attachment uploads** are staged in `/tmp/bbupload-<pid>-<ms>-<n>`
  created `O_EXCL` with mode 0600, written as the multipart body streams
  in, handed to Messages by path, and deleted right after the send is
  dispatched, or on any failure, including the socket dropping mid-upload.
- **Large replies** (contacts with avatars) are staged in `/tmp/bbresp-*`
  files, mode 0600, and unlinked immediately after the server opens them
  for streaming; the bytes live only in the open file descriptor.
- **Nothing leaves the device** except in response to an authenticated
  request from your client. There is no telemetry, no update check, no
  cloud component.

## What it will not do

- It never fakes a feature iOS 9 lacks. Requests for effects, subjects,
  replies, reactions, edits, or attributed bodies get a `400` naming the
  limitation, so the client never believes something happened that did
  not.
- It never deletes messages or chats (those routes are intentionally not
  implemented).

## Hardening notes for contributors

- Every request buffer is bounded and every parser rejects on overflow
  rather than truncating: 16 KB heads, 64 KB buffered bodies, 20 MB
  streamed uploads, 20 MB downloads, 8 KB IPC arguments, 32-deep JSON.
- The SpringBoard side never interprets JSON from the bridge without
  validating it first; a malformed reply becomes a `500` to the client.
- The bridge only accepts upload paths under `/tmp/bbupload-` and only
  regular files.
- Private-API calls are signature-checked and wrapped in `@try/@catch`;
  a missing selector degrades to an error reply, not a crash of
  Messages.app or SpringBoard.
- The host test suite runs under AddressSanitizer/UBSan for the C modules.

## Reporting

Open a GitHub issue for anything that is not a credential leak. For
something sensitive, use GitHub's private vulnerability reporting on the
repository.
