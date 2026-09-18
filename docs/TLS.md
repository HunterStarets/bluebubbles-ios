# Optional HTTPS

Off by default. The desktop and Android clients are happy with plain HTTP
on a LAN. You need HTTPS in exactly one common case: **using the hosted web
app** (`https://bluebubbles.app/web`) against a device on your LAN. Browsers
block a page loaded over HTTPS from talking to an `http://` address
(mixed content), so the server has to speak HTTPS and the browser has to
trust its certificate.

For remote access you do **not** need this: a tunnel (Cloudflare Tunnel,
ngrok, zrok) on another machine terminates TLS itself; see
[INSTALL.md](INSTALL.md#remote-access).

## 1. Make a certificate

On any computer with OpenSSL, with `<device-ip>` your device's LAN address:

```sh
openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes \
  -keyout server.key -out server.crt \
  -subj "/CN=bluebubbles-ios" \
  -addext "subjectAltName=IP:<device-ip>"
openssl pkcs12 -export -inkey server.key -in server.crt \
  -out server.p12 -passout pass:
```

The empty export password matches the server's default; set one and enter
it in Settings if you prefer.

## 2. Put it on the device

Copy `server.p12` to `/var/mobile/Library/BlueBubbles/server.p12` (create
the folder), or anywhere you like and enter the path in Settings.

```sh
ssh root@<device-ip> 'mkdir -p /var/mobile/Library/BlueBubbles'
scp server.p12 root@<device-ip>:/var/mobile/Library/BlueBubbles/server.p12
ssh root@<device-ip> 'chown mobile /var/mobile/Library/BlueBubbles/server.p12; chmod 600 /var/mobile/Library/BlueBubbles/server.p12'
```

## 3. Turn it on

Settings → BlueBubbles iOS → **HTTPS** on. The listener reopens at once;
the server address in the pane changes to `https://…`. If the file is
missing the pane shows `http://… (no certificate)`; if it exists but cannot
be imported (wrong passphrase, not PKCS#12) the server stays on HTTP and
`/tmp/bbdiag-sb.log` shows `transport-listening` without a TLS listener;
check the passphrase and the export step.

## 4. Trust it in the browser

Open `https://<device-ip>:1234/api/v1/ping` in the browser once and accept
the warning, or import `server.crt` into your OS/browser trust store so
there is no warning at all. Then enter `https://<device-ip>:1234` in the web
app.

## Notes

- The certificate is self-signed: it encrypts the connection but does not
  prove the server's identity to anyone who hasn't trusted it explicitly.
  That is fine on a LAN; it is not a substitute for a tunnel on the
  internet.
- iOS 9's TLS stack negotiates TLS 1.2 with RSA certificates; ECDSA keys
  are not recommended.
- The device probe pins the certificate fingerprint on request
  (`BLUEBUBBLES_CERT_SHA256`), see [TESTING.md](TESTING.md).
