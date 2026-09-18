# Compatibility

What the stock BlueBubbles clients can do against this server, route by
route, and what is possible on iOS 9 at all. Reference: BlueBubbles server
at the commit the API was read from (`f2e22862`), client `e2eaced6`.
Verified on an iPad mini 1, iOS 9.3.5, with the desktop client.

Legend: ✅ built and device-verified · 🟢 built and host-tested, not yet
seen on a device · 🟡 partial · ⬜ not built, feasible on iOS 9 · ✖
impossible on iOS 9 (never faked; the client gets a clear error) · ➖ not
applicable (macOS/iCloud/server-management features the client treats as
optional).

"Stock client uses" marks what the unmodified client calls in normal use.

## In plain terms

**Works:** connect and sync every iMessage/SMS conversation with names and
photos; read full history with delivery/read state; view and save
attachments the device has; send texts, photos, and files; start new
one-to-one conversations.

**Built, unconfirmed on a device:** incoming messages appearing live in the
client (`new-message` pushes); starting a group conversation.

**Not yet:** marking a chat read (the iPad's unread badge stays), typing
indicators, renaming/adding/removing/leaving groups, deleting, fetching
attachments the device hasn't downloaded, blurhash placeholders,
incremental sync counts.

**Never (OS level):** reactions/tapbacks, effects, edit, unsend, inline
replies, mentions, text+attachment in one bubble, Live Photos, audio
*messages* (audio files send as plain attachments), Focus, FaceTime, Find
My, scheduling.

## REST `/api/v1`

### General / server
| Route | Stock client uses | Ours | Notes |
|---|---|---|---|
| `GET /ping` | yes | ✅ | |
| `GET /server/info` | yes (startup) | ✅ | Conservative profile: server_version 0.2.0, private_api false |
| `GET /server/logs` | settings page | ➖ | |
| `GET /server/restart/soft`, `/restart/hard` | settings page | ➖ | |
| `GET /server/update/check`, `POST /server/update/install` | settings page | ➖ | No server updates |
| `GET /server/alert`, `POST /server/alert/read` | settings page | ➖ | |
| `GET /server/statistics/totals`, `/media`, `/media/chat` | settings page | ⬜ | Could be derived from chat/message counts |
| `POST /macos/lock`, `POST /macos/imessage/restart` | settings page | ➖ | |
| `GET /icloud/account`, `POST /icloud/account/alias`, `GET /icloud/contact` | settings page | ➖ | |
| `GET /findmy/devices`, `/friends` (+ refresh) | FindMy page | ➖ | Not promised |
| `POST /fcm/device`, `GET /fcm/client` | yes (startup, non-gating) | ✅ | 404 by design; client continues on LAN |
| `GET /` (web UI) | no | ➖ | |

### Chats
| Route | Stock client uses | Ours | Notes |
|---|---|---|---|
| `GET /chat/count` | yes | ✅ | iMessage/SMS breakdown |
| `POST /chat/query` | yes (sync) | ✅ | participants + lastMessage, paging |
| `GET /chat/:guid` | yes (chat creator lookup) | ✅ | Alias/identifier lookup incl. not-yet-listed chats |
| `GET /chat/:guid/message` | yes (history) | ✅ | Sorted, windowed, receipts, handles |
| `POST /chat/new` | yes (chat creator) | ✅ | One recipient verified; group (`chatForIMHandles:`) wired, unproven |
| `PUT /chat/:guid` (rename group) | group details | ⬜ | Needs `IMChat setDisplayName:`-style probe |
| `POST /chat/:guid/read` | yes (every chat open) | ⬜ | `markAllMessagesAsRead`; cheap, next up |
| `POST /chat/:guid/unread` | chat list swipe | ⬜ | Selector unknown |
| `POST /chat/:guid/leave` | group details | ⬜ | Probe |
| `POST/DELETE /chat/:guid/participant`, `/participant/add`, `/participant/remove` | group details | ⬜ | Probe |
| `POST/DELETE /chat/:guid/typing` | yes (while typing, private API on) | ⬜ | Client only sends with private_api true; probe |
| `GET/POST/DELETE /chat/:guid/icon` | group details | ⬜ | Read side maybe; set/remove needs probe |
| `DELETE /chat/:guid`, `DELETE /chat/:guid/:messageGuid` | delete actions | ⬜ | Intentionally disabled for now |
| `GET /chat/:guid/share/contact/status`, `POST .../share/contact` | rare | ➖ | Nickname sharing (iOS 17+) |

### Messages
| Route | Stock client uses | Ours | Notes |
|---|---|---|---|
| `POST /message/text` | yes | ✅ | tempGuid echo; effects/subject/replies → 400 |
| `POST /message/attachment` | yes | ✅ | Streamed multipart; image, 2 MB photo, audio file verified |
| `POST /message/multipart` | private API only | ✖ | Mixed text+attachment parts; iOS 9 has no equivalent |
| `POST /message/react` | private API only | ✖ | Tapbacks: iOS 10+ |
| `GET /message/count` | yes | ✅ | |
| `GET /message/count/updated` | yes (incremental sync) | ⬜ | Answer with `{total}` over updated messages; cheap |
| `GET /message/count/me` | statistics | ⬜ | |
| `POST /message/query` | yes (sync) | ✅ | Date windows, withChats |
| `GET /message/:guid` | yes (rare) | 🟢 | Only messages already loaded in memory |
| `POST /message/:guid/edit`, `/unsend` | private API only | ✖ | iOS 16+ |
| `POST /message/:guid/notify` | private API only | ✖ | |
| `GET /message/:guid/embedded-media` | digital touch/handwriting | ✖ | |
| `GET/POST/PUT/DELETE /message/schedule…` | scheduling page | ➖ | Not promised |

### Attachments
| Route | Stock client uses | Ours | Notes |
|---|---|---|---|
| `GET /attachment/count` | statistics | ⬜ | |
| `POST /attachment/upload` | private API multipart | ✖ | Only feeds `/message/multipart` |
| `GET /attachment/:guid` | yes | ✅ | |
| `GET /attachment/:guid/download` | yes | ✅ | Raw bytes; 404 undownloaded; 413 > 20 MB |
| `GET /attachment/:guid/download/force` | yes (undownloaded) | ⬜ | Trigger the transfer on the iPad, then serve |
| `GET /attachment/:guid/blurhash` | yes (placeholder) | ⬜ | Needs a native blurhash; client falls back |
| `GET /attachment/:guid/live` | Live Photos | ✖ | iOS 9 has no Live Photo attachments |

### Handles / contacts
| Route | Stock client uses | Ours | Notes |
|---|---|---|---|
| `GET /handle/count`, `POST /handle/query`, `GET /handle/:guid` | rare | ⬜ | Handles already come with chats |
| `GET /handle/availability/imessage` | chat creator (service pick) | ⬜ | `IMHandle` availability probe; client defaults to iMessage |
| `GET /handle/availability/facetime` | | ➖ | |
| `GET /handle/:guid/focus` | | ➖ | Focus: iOS 15+ |
| `GET /contact` | yes | ✅ | All contacts + avatars (staged file) |
| `POST /contact/query` | yes | 🟡 | Returns all contacts; address filter not applied |

### FaceTime, themes, settings, webhooks
| Route | Stock client uses | Ours | Notes |
|---|---|---|---|
| `POST /facetime/session`, `/answer/:id`, `/leave/:id` | | ➖ | |
| `GET/POST/DELETE /backup/theme`, `/backup/settings` | settings backup | ➖ | Client stores locally if absent |
| `DELETE /webhook/:id` | | ➖ | |

## Socket.IO events (client → server, acknowledged)
| Event | Stock client uses | Ours | Notes |
|---|---|---|---|
| `get-server-metadata` | yes | ✅ | |
| `get-chats`, `get-chat`, `get-chat-messages`, `get-messages`, `get-attachment` | no (REST) | 🟢 | Routed to the same bridge ops |
| `start-chat` | no (REST) | 🟢 | → chat.new |
| `send-message`, `send-message-chunk` | no (REST) | ⬜ | Older socket send path |
| `get-attachment-chunk`, `get-last-chat-message`, `get-participants` | no | ⬜ | |
| `mark-chat-read`, `toggle-chat-read-status`, `open-chat` | no (REST) | ⬜ | With mark-read |
| `started-typing`, `stopped-typing`, `update-typing-status` | no (REST) | ⬜ | With typing |
| `rename-group`, `add-participant`, `remove-participant` | no (REST) | ⬜ | With group mgmt |
| `send-reaction` | | ✖ | |
| `save-vcf`, `get-vcf`, `get-contacts-from-vcf` | | ➖ | |
| `get-server-config`, `change-proxy-service`, `add-fcm-device`, `get-fcm-client`, `get-logs`, `restart-*`, `check-for-server-update` | | ➖ | |

## Push events (server → client)
| Event | Ours | Notes |
|---|---|---|
| `new-message` | 🟢 | Emitted from `_messageReceived_`; not yet confirmed live |
| `updated-message` | 🟢 | From `_chatItemsDidChange_`, deduped |
| `message-send-error` | 🟡 | Allowlisted, no emitter |
| `chat-read-status-changed` | 🟡 | Allowlisted; emit with mark-read |
| `hello-world` | 🟡 | Allowlisted, unused |
| `typing-indicator` | ⬜ | With typing |
| `group-name-change`, `participant-added/-removed/-left` | ⬜ | With group mgmt |
| `new-server`, `server-update*` | ➖ | |
| `incoming-facetime`, `ft-call-status-changed`, `new-findmy-location`, `imessage-aliases-removed`, `scheduled-message-*` | ➖ | |

## Summary

- Everything the stock client calls on startup, sync, reading, and
  sending is ✅ except mark-read (called on every chat open, currently
  404, harmless but noisy) and `message/count/updated`.
- ⬜ feasible and worth doing, roughly in order: mark-read/unread,
  `message/count/updated`, address-filtered `contact/query`, typing,
  group rename/add/remove/leave, `download/force`, statistics.
- ✖ is fixed by the OS: reactions, effects, edit/unsend, replies,
  mentions, multipart mixed sends, Live Photos, embedded media, focus.
- ➖ is server-management surface (macOS, iCloud, FindMy, FaceTime,
  updates, backups, webhooks, scheduling) the client treats as optional.

## Porting effort by functionality

For everything not yet built the question is whether the iOS-side API is
known on iOS 9.3.5.

Tiers: **Ported** (device-proven on the reference device) · **A** no discovery (server-
side work on data we already have) · **B** known selector, one try
(well-known IMCore/ChatKit private API; dynamic check, 501 fallback, one
device run) · **C** probe first (a read-only selector probe build before
implementing).

| Functionality | Status | Tier | Evidence / unknown |
|---|---|---|---|
| Connect, sync, list, history, receipts | ✅ | Ported | |
| Contacts names/photos | ✅ | n/a | Built on AddressBook |
| Attachment download | ✅ | Ported | getfile |
| Send text / attachment | ✅ | Ported | send / sendfile |
| New 1:1 conversation | ✅ | Ported | newchat |
| New group conversation | 🟢 | B | `chatForIMHandles:` confirmed present by a selector probe, never called |
| Live incoming messages | 🟢 | B | Hooks exist; needs the other-phone check |
| Mark read | ⬜ | B | `IMChat markAllMessagesAsRead` |
| `message/count/updated` | ⬜ | A | |
| Filtered `contact/query` | ⬜ | A | |
| Statistics | ⬜ | A | |
| Blurhash | ⬜ | A | Pure computation; public CGImage decode |
| `message/:guid` for unloaded chats | 🟡 | A | Load the chat first |
| iMessage/SMS availability | ⬜ | C | ID status query class on iOS 9 |
| Typing out | ⬜ | B | `IMChat setLocalUserIsTyping:` |
| Typing in | ⬜ | C | Notification/hook unknown |
| Read-state push (`chat-read-status-changed`) | ⬜ | C | Unread-count change source unknown |
| Mark unread | ⬜ | C | No known selector |
| Rename group | ⬜ | C | `setDisplayName:` variants |
| Add/remove participant, leave | ⬜ | C | `addParticipants:reason:` family signatures |
| Group photo | ⬜ | C | May not exist on iOS 9 |
| Force-download attachment | ⬜ | C | `IMFileTransferCenter` accept/resume |
| Delete message/chat | ⬜ | C | Deliberately off |
| Reactions/effects/edit/unsend/replies/mentions/Live Photo/mixed | ✖ | n/a | Not in iOS 9 |

Recommended order: ship the A/B rows in one candidate (mark-read, typing
out, group creation check, count/updated, contact filter); then one
read-only selector probe build that answers every C unknown in a single
install; then implement what it confirms.
