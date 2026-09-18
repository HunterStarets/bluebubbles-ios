# Testing

Three layers: host tests that need no device, a read-only probe you run
against a real device, and the manual send tests.

## Host suite

```sh
cd tests
make clean && make check
```

C suites (`-std=c99 -Wall -Wextra -Werror`): `websocket`, `http`,
`socketio`, `json`, `serialize`, `multipart`, `connection`, `engineio`,
`bridge`. They exercise every module the SpringBoard dylib is built from,
including fragmentation, pipelining, pending responses, body streaming,
request correlation, timeouts, and every validation path of the router.

Node (`node --test`): `bluebubbles-fixture-server.js` is a deterministic
stand-in for this server that speaks the same routes and schemas;
`bluebubbles-contract-test.js` drives it through the stock client's
startup sequence, REST auth, multipart uploads, chat creation, and the
Engine.IO websocket/polling transcripts.

Sanitizers: build any C test with
`-fsanitize=address,undefined -g -O1` and run it; the suite is clean.

`stock-client-startup-source-transcript.json` records, structurally, the
route sequence the unmodified client issues on first connect; it is what
the fixture and the probe are checked against.

## Device probe (read-only)

`tests/live-bluebubbles-probe.js` runs 17 steps against a device and prints
only schema-level facts: status codes, counts, field types, timings. It
never prints an address, name, message text, guid, or the password, and it
never creates or sends anything; its two POSTs (`/chat/new` with `{}`,
`/message/attachment` with a JSON body) are refused by validation before
any bridge work, which is exactly what they verify.

```sh
read -r -s -p 'password: ' BLUEBUBBLES_PASSWORD; printf '\n'
BLUEBUBBLES_HOST=<device-ip> BLUEBUBBLES_PASSWORD="$BLUEBUBBLES_PASSWORD" \
  node tests/live-bluebubbles-probe.js
unset BLUEBUBBLES_PASSWORD
```

Options: `BLUEBUBBLES_PORT` (1234), `BLUEBUBBLES_SCHEME=https` when TLS is
on, `BLUEBUBBLES_CERT_SHA256=<fingerprint>` to pin the certificate (the
first run prints it).

Expected on a healthy device: every step `PASS`; `chat-query` reports
`withParticipants` equal to `count` and `zeroRowId: 0`; `chat-history`
reports `dateLooksLikeMs: true`, `sortedDesc: true`;
`attachment-download` is `raw-bytes` (or `not-downloaded-404` if the sampled
transfers were never downloaded); `chat-new-rejects-empty` and
`attachment-send-rejects-json` are `status: 400`.

The same probe runs against the fixture for a self-check:

```sh
PORT=18234 node tests/bluebubbles-fixture-server.js &
BLUEBUBBLES_HOST=127.0.0.1 BLUEBUBBLES_PORT=18234 BLUEBUBBLES_PASSWORD=fixture-only \
  node tests/live-bluebubbles-probe.js
```

## Installing a build on a test device

Every command is run by you, one at a time. Nothing here sends a message.

```sh
DEVICE_IP=<device-ip>
PKG=packages/com.hunterstarets.bluebubbles-ios_<version>_iphoneos-arm.deb
sha256sum "$PKG"
```

Transfer and verify the hash on the device (iOS 9's sshd needs the RSA
options):

```sh
ssh -oHostKeyAlgorithms=+ssh-rsa -oPubkeyAcceptedAlgorithms=+ssh-rsa root@"$DEVICE_IP" \
  'cat >/tmp/bluebubbles-ios.deb' < "$PKG"
ssh -oHostKeyAlgorithms=+ssh-rsa -oPubkeyAcceptedAlgorithms=+ssh-rsa root@"$DEVICE_IP" \
  'openssl dgst -sha256 /tmp/bluebubbles-ios.deb'
```

Install and respring:

```sh
ssh -oHostKeyAlgorithms=+ssh-rsa -oPubkeyAcceptedAlgorithms=+ssh-rsa root@"$DEVICE_IP" \
  'dpkg -i --force-overwrite /tmp/bluebubbles-ios.deb && killall SpringBoard'
```

Wait for the lock screen, confirm it is not in safe mode, open Messages.app
once, then check the traces:

```sh
ssh -oHostKeyAlgorithms=+ssh-rsa -oPubkeyAcceptedAlgorithms=+ssh-rsa root@"$DEVICE_IP" \
  'ps -e | grep -c "[S]ubstrateSafeMode"; tail -n 3 /tmp/bbdiag-sb.log; tail -n 3 /tmp/bbdiag-sms.log'
```

Expect `0`, a `transport-listening` line, and a `receiver-ready` line. Then
run the probe.

## Manual send tests

Sending is a real outbound action. Test only against your own number or
address, one message at a time, with the client:

1. **Text** into an existing chat: appears once (no duplicate), delivers.
2. **Attachment**: a small image, then a multi-megabyte photo (exercises the
   streamed upload path), then a non-image file. Afterwards
   `ls /tmp/bbupload-*` on the device must list nothing.
3. **New chat** to an address with no existing thread: the client shows
   "Finding or creating chat…", opens the conversation, the message
   delivers. Note the created chat is listed by `chat/query` only once it
   holds a message.
4. **Live events**: with the client open, message the device from another
   phone; the message should appear without a refresh.

Count-only trace checks after sends:

```sh
grep -c send-dispatched /tmp/bbdiag-sms.log; grep -c send-failed /tmp/bbdiag-sms.log
```

## Rollback

Keep the previous `.deb`. `dpkg -i` the older version and respring. If
SpringBoard boot-loops or enters safe mode, see
[TROUBLESHOOTING.md](TROUBLESHOOTING.md).
