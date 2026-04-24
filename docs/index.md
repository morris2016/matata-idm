---
title: matata
description: A Windows download manager. IDM-style, free, open source.
---

# matata

**matata** is a Windows download manager — multi-connection segmented
downloads, resume, browser integration, HLS / DASH video grabber, and a
classic Win32 UI. Written from scratch in C++17 with no MFC/ATL.

The browser extension is a bridge: it intercepts downloads from
Chrome / Edge / Firefox and hands them to the native matata app.

---

## Install

**1. Install the app**

Grab the latest installer from the
[releases page](https://github.com/morris2016/matata-idm/releases) and run it.
The installer sets up four components:

- `matata.exe` — command-line client
- `matata-gui.exe` — IDM-style GUI
- `matata-host.exe` — Native Messaging host for the browser extension
- `matata_shell.dll` — Explorer right-click integration

**2. Install the browser extension**

- **Chrome** — [Chrome Web Store listing](#){: rel="noreferrer"}
- **Edge**   — [Edge Add-ons listing](#){: rel="noreferrer"}
- **Firefox** — [Firefox AMO listing](#){: rel="noreferrer"}

After installing the extension, the matata installer has already
registered the native-messaging host; just click the toolbar icon →
**ping native host** to confirm the handshake.

---

## Features

- **Multi-connection segmented HTTP / HTTPS** — up to 32 parallel
  connections per file with dynamic segment stealing so the last few
  percent don't crawl.
- **Resume** — interrupted downloads keep a `.mtpart` sidecar; re-run
  the same URL to continue.
- **HLS / DASH video grabber** — paste a `.m3u8` or `.mpd` URL, pick a
  quality, done. AES-128 encrypted HLS supported.
- **FTP / FTPS** — handwritten Winsock + SChannel TLS. `ftp://` and
  `ftps://` URLs are auto-detected.
- **Browser integration** — Manifest V3 extension for Chromium +
  Firefox. A live "Active downloads" popup reports progress as matata
  reports it over Native Messaging. Drag-and-drop URLs onto the main
  window.
- **Checksum verify** — if the server advertises `Digest:` or
  `Content-MD5:`, matata hashes the result and checks.
- **Scheduler** — `--start-at` / `--stop-at`, plus a GUI Scheduler
  dialog.
- **Bandwidth cap** — global token-bucket rate limit.
- **Queue + auth** — multi-URL queue with per-download concurrency cap,
  Basic / Bearer / `.netrc` auth.

---

## Links

- **Source**: [github.com/morris2016/matata-idm](https://github.com/morris2016/matata-idm)
- **Privacy policy**: [privacy](./privacy)
- **Support / report an issue**: [support](./support)

---

*matata is free software. Independent of Tonec Inc. / Internet Download
Manager — designed from scratch on public standards (RFC 7233 range
requests, RFC 4217 FTPS, RFC 8216 HLS, MPEG-DASH / ISO 23009, etc.).*
