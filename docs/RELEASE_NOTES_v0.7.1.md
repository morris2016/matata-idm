# matata v0.7.1

First public release. Paste this into the GitHub Release body:

---

## Download

**Windows 10/11 x64** — **[matata-0.7.1-setup.exe](../../releases/download/v0.7.1/matata-0.7.1-setup.exe)** (~2.3 MB)

Single-file Inno Setup installer. Installs `matata.exe` (CLI),
`matata-gui.exe` (IDM-style Win32 GUI), `matata-host.exe` (Native
Messaging host for the browser extension), and `matata_shell.dll`
(Explorer right-click integration). HKCU-only — **no administrator
required**.

### Browser extensions

- **Chrome / Chromium** — *pending listing approval*. For now you can
  load `extension/` unpacked in `chrome://extensions` with Developer
  mode on, then run `install-extension.bat <EXTENSION_ID>`.
- **Microsoft Edge Add-ons** — *pending listing approval*.
- **Firefox (AMO)** — *pending listing approval*.

---

## What's in v0.7.1

**Engine**

- Segmented HTTP/HTTPS via WinHTTP with dynamic segment stealing so the
  last few percent don't crawl.
- Resume via `.mtpart` sidecar, etag / last-modified validation.
- HLS (RFC 8216) with AES-128-CBC decryption and live-playlist polling;
  DASH (MPEG-DASH) with `SegmentTemplate` + `SegmentList` + cascaded
  `<BaseURL>` and per-representation quality picker. Optional `ffmpeg`
  remux / video+audio mux.
- FTP (RFC 959) and FTPS (RFC 4217 `AUTH TLS`) via handwritten Winsock +
  Windows SChannel with a process-wide credential for session resumption.
- Checksum verify on completion when the server advertises `Digest:` or
  `Content-MD5:` (MD5, SHA-1, SHA-256, SHA-384, SHA-512 via BCrypt).
- Auth: Basic, Bearer, netrc; custom headers (`-H`), cookie / referer /
  user-agent flags.
- Scheduler with start-at / stop-at, bandwidth cap (token bucket),
  category routing, multi-URL queue with per-download + total connection
  caps, plain-text queue state file.

**GUI (`matata-gui.exe`)**

- Pure Win32 + common controls. No MFC or ATL.
- Category tree + 8-column ListView (File / Q / Size / Status / Time left
  / Transfer rate / Last Try Date / Description) with per-row file icons
  (`SHGetFileInfo`).
- Real Options + Scheduler dialogs, all settings persisted to
  `HKCU\Software\matata\Gui`.
- System-tray icon with minimize-to-tray and balloon notifications.
- Clipboard URL watcher — prompts on fresh HTTP(S) URLs.
- OLE drop target — drag URL text or files onto the window.
- Command-line URL is auto-queued; `matata://URL` scheme lets anything
  on the system launch a download.

**Browser integration**

- Manifest V3 WebExtension (Chrome / Edge / Firefox — same zip; Firefox
  picks up `browser_specific_settings.gecko.id`).
- Service worker intercepts matching downloads and hands URL + cookies +
  referer to `matata-host.exe` over Native Messaging.
- Bi-directional progress: popup's "Active downloads" updates in real
  time as `matata.exe --json-progress` reports.
- Browser media sniffer — `chrome.webRequest.onBeforeRequest` lists
  `.m3u8` / `.mpd` requests per-tab so you can grab them with one click.
- Persistent port via `chrome.runtime.connectNative`.

**Shell / packaging**

- COM shell extension (`matata_shell.dll`) — "Download with matata" on
  the right-click menu for files, folders, and Explorer background.
- `matata:` URL-scheme handler + SendTo shortcut via HKCU.
- Inno Setup installer (HKCU — no admin).

**Ecosystem foundations (not wired into a full product yet)**

- `.torrent` bencode parser with SHA-1 info-hash via BCrypt; magnet URI
  parser with hex-encoded info hash. Tracker + peer-wire protocols are
  out of scope for this release.
- Auto-update client: `matata --check-update <manifest-url>` compares
  against a JSON manifest. Needs a real update server.

---

## Verified against

- **HTTP/HTTPS** — proof.ovh.net (1 MB / 10 MB / 100 MB), OVH + Mux
  HLS, Akamai BBB 30fps DASH.
- **HLS AES-128** — `test-streams.mux.dev/test_001/stream.m3u8` (64
  segments, 64 MiB output, MPEG-TS sync bytes at packet boundaries).
- **DASH** — `dash.akamaized.net/akamai/bbb_30fps/bbb_30fps.mpd` (160
  fragments + init, 15 MiB output, valid fMP4 ftyp box).
- **FTP** — `ftp.gnu.org/README` and `hello-2.12.2.tar.gz` (valid
  gzip).
- **FTPS** — `ftps://demo:password@test.rebex.net/readme.txt` (379
  bytes, AUTH TLS + PROT P + session resumption).

---

## Known gaps

- **SFTP** — not implemented. Would need libssh2 linked in; a concrete
  plan is in the README.
- **Full BitTorrent engine** — only the foundational parsers are here.
  Tracker + peer wire protocol + piece verification are future work
  (concrete plan in README).

---

## Build from source

Needs Visual Studio 2022 Build Tools (C++ toolset + Windows 10/11 SDK).

```
git clone https://github.com/morris2016/matata-idm
cd matata-idm
build.bat
```

Produces the four artifacts in `build/`. The installer is built
separately with Inno Setup 6:

```
"C:\Users\<you>\AppData\Local\Programs\Inno Setup 6\ISCC.exe" installer\matata.iss
```
