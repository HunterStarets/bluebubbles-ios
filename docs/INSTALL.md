# Install and connect

## Requirements

- A jailbroken device running iOS 9.0–9.3.5. Developed on an iPad mini 1
  (armv7, iOS 9.3.5); any 32-bit or 64-bit device on iOS 9 should work, but
  only the iPad mini 1 has been tested. iOS 10 and newer are not supported.
- Cydia/Sileo with the default repos, so the dependencies resolve:
  `mobilesubstrate`, `com.rpetrich.rocketbootstrap`, `preferenceloader`.
- The device and the client on the same LAN (or a tunnel, see below).

## Install the package

Download `com.hunterstarets.bluebubbles-ios_<version>_iphoneos-arm.deb`
from the GitHub release, then either:

- open it in **Filza** and tap Install, or
- copy it to the device and run, as root over SSH:

  ```sh
  dpkg -i com.hunterstarets.bluebubbles-ios_<version>_iphoneos-arm.deb
  killall SpringBoard
  ```

If `dpkg` complains about missing dependencies, install them from
Cydia/Sileo first (`apt-get -f install` also works on most setups).

## First run

1. After the respring, open **Settings → BlueBubbles iOS**.
2. The pane shows:
   - **Server address**: `http://<device-ip>:1234`. This is what you type
     into the client.
   - **Password**: generated randomly on first launch. Edit it if you want
     your own; the server picks the change up immediately.
   - **Enabled**: turn the server off without uninstalling.
   - **HTTPS**: off by default; see [TLS.md](TLS.md).
3. Open **Messages.app** once. The part of the server that reads and sends
   messages runs inside Messages; until it has been opened after a respring,
   the client can connect but every chat request answers
   `503 The messaging bridge is unavailable`. iOS keeps Messages resident
   afterwards; you don't need to keep it in the foreground.

## Connect a client

In the BlueBubbles desktop, Android, or web client choose **manual setup**
(not the QR code), enter the server address and the password, and connect.
The client performs its normal full sync: chat list, then history per chat,
then contacts.

The client's *Private API* features stay off: the server advertises
`private_api: false`, so the client hides reactions, effects, and other
things iOS 9 cannot do.

## Remote access

The device only listens on your LAN. For access from outside, do what the
stock BlueBubbles server does and put a tunnel in front of it, on another
machine on the same network (a Raspberry Pi or any always-on box), since the
tunnel agents don't run on an iOS 9 device:

- **Cloudflare Tunnel**: `cloudflared tunnel --url http://<device-ip>:1234`
  (or a named tunnel for a stable hostname). Point the client at the
  `https://…` hostname Cloudflare gives you.
- **ngrok** / **zrok** work the same way.

The tunnel terminates TLS; the device keeps serving plain HTTP on the LAN.
Nothing on the device changes.

## Update

Install the new `.deb` over the old one (`dpkg -i`) and respring. Settings
are kept.

## Uninstall

Remove it from Cydia/Sileo, or `dpkg -r com.hunterstarets.bluebubbles-ios`,
then respring. The preferences file
(`/var/mobile/Library/Preferences/com.hunterstarets.bluebubbles-ios.plist`)
is left in place so a reinstall keeps your password; delete it by hand if
you want a clean slate.
