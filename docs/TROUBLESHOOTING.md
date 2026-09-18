# Troubleshooting

## The client connects but every chat request fails with 503

"The messaging bridge is unavailable": Messages.app has not been opened
since the last respring, so the MobileSMS half is not running. Open
Messages once. If it persists, check `/tmp/bbdiag-sms.log` for
`receiver-ready`; if it is missing, RocketBootstrap is not installed or the
tweak did not load into MobileSMS.

## The client cannot connect at all

- Settings → BlueBubbles iOS → is **Enabled** on? Is the address the one
  the pane shows (the device's current LAN IP; it changes with DHCP)?
- `http://` vs `https://`: use `http://` unless you turned HTTPS on.
- `/tmp/bbdiag-sb.log` should contain `transport-listening`. If it shows
  `transport-start-failed`, port 1234 is taken or the password file is
  unreadable.
- Firewall/AP isolation on your Wi-Fi can block LAN clients from each other.

## 401 Unauthorized

The password in the client does not match Settings. The comparison is
exact and case-sensitive.

## SpringBoard boot-loops or is in safe mode after install

Hold Volume Up while it boots to enter safe mode, then in SSH:

```sh
dpkg -r com.hunterstarets.bluebubbles-ios
killall SpringBoard
```

Crash logs are in `/var/mobile/Library/Logs/CrashReporter/`; an issue with
the SpringBoard or MobileSMS crash log attached (they contain no message
content) is the fastest way to get it fixed.

## Semi-untethered jailbreaks

If the device reboots, the jailbreak must be re-applied before the tweak
loads (and before SSH works). The server is simply absent until then.

## Everything answered 504 after a while, until Messages was reopened

Messages.app was killed. On a 512 MB device iOS may kill it under memory
pressure (no crash log is written for that), most likely during a first
full sync of a large history while something else is loading. Reopen
Messages; the bridge comes back at once. If it happens repeatedly, open an
issue with the trace tails and the size of your history (`chat/count`,
`message/count` from the probe).

## Messages are slow to appear in the client

Incoming-message pushes require the client's Socket.IO connection to be up
(the client reconnects on its own) and Messages.app to be resident. A full
sync of a large history on an A5-class device takes a while; the client
shows progress.

## Attachments show as missing

The device serves only attachments it has already downloaded. Open the
conversation on the device so iOS fetches the attachment, then retry from
the client. Files over 20 MB are refused with `413`.

## Where the logs are

`/tmp/bbdiag-sb.log` (SpringBoard) and `/tmp/bbdiag-sms.log` (MobileSMS):
one fixed event name per line, no content. They are safe to paste into an
issue. They are capped at 64 KB and cleared on reboot.
