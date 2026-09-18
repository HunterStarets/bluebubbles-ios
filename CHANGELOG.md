# Changelog

## 0.0.1: first release

An unofficial BlueBubbles server for jailbroken iOS 9. Verified end to end
with the stock desktop client on an iPad mini 1 / iOS 9.3.5.

- REST `/api/v1` and Socket.IO (WebSocket and polling) served from
  SpringBoard on port 1234, plain HTTP; optional TLS with a user-supplied
  certificate.
- Full sync: chat list with participants and last message, per-chat
  history with delivery/read state and sender handles, message counts and
  queries, stable guids and ids, Unix-ms dates.
- Contacts with phone numbers, emails, and photos from the device address
  book.
- Attachment metadata and download.
- Send text into existing chats, send attachments (streamed multipart
  upload), create new one-to-one conversations.
- `new-message` / `updated-message` pushes (host-tested; device
  confirmation pending).
- Settings pane: server address, auto-generated password, enable toggle,
  HTTPS.
- Count-only diagnostics; private staged files for uploads and large
  replies; host test suite with sanitizers; read-only device probe;
  package and module audits.

Not in this release: mark read/unread, typing indicators, group
management, delete, force-download, blurhash. See `docs/COMPATIBILITY.md`.
