# matata

A Windows download manager, written from scratch in C++. Goal: reach (and pass)
IDM feature parity while being free and open.

> The name is Swahili for "trouble" — as in _"hakuna matata"_, no trouble
> finishing your downloads.

## Status: v0.7.1 — FTPS, BitTorrent foundation, installer, auto-update

Works today:

- **Multi-connection segmented HTTP/HTTPS download** (WinHTTP, up to 32 parallel
  connections per file).
- **Dynamic segment stealing.** Fast workers carve tails off the slowest
  remaining segment so the last few percent no longer run single-connection.
  Segment count grows at runtime (verified 8 → 14 on a 100 MiB test).
- **HTTP/2** enabled on the session when the OS supports it.
- **Resume from interruption.** `.mtpart` + `.mtmeta` sidecar; etag /
  Last-Modified validated; re-running the same command picks up.
- **Range-support auto-detection** via `Range: bytes=0-0` probe.
- **Custom request headers**: `--cookie`, `--referer`, `--user-agent`,
  repeatable `-H 'K: V'`.
- **HTTP auth**: `--user USER:PASS` (Basic), `--bearer TOKEN`, or `--netrc` /
  `--netrc-file PATH` to look up credentials by host.
- **Server-advertised checksum verification** (via `Digest:` / `Content-MD5:`
  response headers) using BCrypt (MD5/SHA-1/SHA-256/SHA-384/SHA-512). Opt out
  with `--no-verify-checksum`.
- **Bandwidth cap** (`--limit-rate`, token bucket, suffixes `k/m/g`).
- **Verbose mode** (`-v`) — probe diagnostic (size, ranges, digest) to stderr.
- **Multi-URL queue** with caps on concurrent downloads (`-j`) and total HTTP
  connections across the queue (`-J`).
- **Queue persistence** (`--queue FILE`) — plain-text state file, reloaded on
  next run.
- **Time-windowed scheduler**: `--start-at` holds the queue until a given time;
  `--stop-at` prevents new dispatches past a given time (in-flight items drain).
  Accepts `HH:MM` (today, or tomorrow if past) or `YYYY-MM-DD HH:MM`.
- **Category routing** (`--categorize`) — IDM-style subfolders
  (Video/Music/Archives/Programs/Documents/Other) by file extension.
- **Shutdown on completion** (`--shutdown-on-done`) — Windows shutdown
  after the queue drains successfully.
- **Redirects**: transparent via WinHTTP.
- **File-name inference** from `Content-Disposition`, then URL path.
- **Ctrl-C cancellation**: partial state flushed to disk before exit.
- **CLI** with live progress (percent, bytes, rate, active connections,
  segment count).
- **Chrome / Edge integration** via a Manifest V3 WebExtension plus a
  stdio Native Messaging host (`matata-host.exe`). When the browser starts a
  download whose extension matches the user's list, the extension cancels the
  browser transfer and hands the URL + cookies + referer to the native host,
  which spawns `matata.exe`. Verified end-to-end with a piped-stdin harness.
- **HLS video grabber**: auto-detects `.m3u8` URLs (or force with `--hls`),
  parses master + media playlists, picks a variant by `--quality`
  (`best` / `worst` / `<height>[p]`), downloads segments in parallel,
  concatenates them to a single `.ts`. Optional `--ffmpeg PATH` remuxes
  to `.mp4`. Verified on the Mux test stream; MPEG-TS sync bytes at
  offsets 0 and 188 confirmed.
- **HLS AES-128 decryption** (`#EXT-X-KEY:METHOD=AES-128`). Keys are fetched
  once per URI, cached; IVs come from the manifest when explicit and are
  otherwise derived from the media-sequence number (per RFC 8216). Uses
  BCrypt AES-CBC. Unsupported modes (SampleAES, unknown) bail with a
  clear error.
- **HLS live playlists**: the grabber polls the manifest every
  ~`EXT-X-TARGETDURATION/2` seconds, queues newly-seen segments onto the
  running worker pool, and exits when `#EXT-X-ENDLIST` appears or the user
  hits Ctrl-C (partial output is kept).
- **DASH grabber** (`.mpd`): hand-rolled XML parser, SegmentTemplate
  substitution (`$Number$`, `$Time$`, `$RepresentationID$`, `$Bandwidth$`,
  incl. printf-style `%0Nd` widths), SegmentList, SegmentTimeline, and
  cascaded `<BaseURL>`s. Picks best video representation (or `--quality
  720p`); downloads `init + media` segments in parallel. With `--ffmpeg`
  it also downloads the best audio representation and muxes video+audio
  into one `.mp4`. Verified on Akamai's BBB 30fps stream: 160 segments
  (init + 159), `ftyp` box at offset 4 of the concatenated fMP4.
- **Browser media sniffer**: the extension's service worker watches
  `chrome.webRequest.onBeforeRequest` for `.m3u8` / `.mpd` requests and
  records them per-tab. The popup shows a "Streams on this tab" section
  with a **grab** button for each detected stream.
- **Bi-directional progress**: `matata.exe --json-progress` emits
  line-delimited JSON events (`start`, `progress`, `done`, `err`, `abort`).
  The native host now spawns each download with a piped stdout and a
  per-job reader thread that forwards every JSON line through the same
  Native Messaging channel back to the extension, plus a final `exit`
  message on process termination. The popup renders a live
  **Active downloads** list with per-job progress bars.
- **Firefox** is supported alongside Chrome, Edge, and Chromium: the
  manifest carries `browser_specific_settings.gecko.id`, and the install
  script writes both `allowed_origins` (Chromium) and `allowed_extensions`
  (Firefox) variants of the native-host manifest, registering them under
  `HKCU\Software\{Google\Chrome,Microsoft\Edge,Chromium,Mozilla}\NativeMessagingHosts`.
- **Win32 GUI** (`matata-gui.exe`): pure Win32 + common controls, no MFC
  or ATL. **IDM-style split layout** — category tree on the left (All
  Downloads → Compressed / Documents / Music / Programs / Video, plus
  Unfinished / Finished / Queues), main ListView on the right with
  eight columns matching IDM's: **File Name / Q / Size / Status / Time
  left / Transfer rate / Last Try Date / Description**. Per-row file
  icons (via `SHGetFileInfo` with `SHGFI_USEFILEATTRIBUTES`, cached by
  extension). IDM-style formatting: "32.23&nbsp;&nbsp;GB", "Complete" or
  "X.XX%", "3 hour(s) 34 min", "Jan 23 18:27:04 2026". Tree selection
  filters the listview. Toolbar: Add URL, Resume, Stop, Stop All,
  Delete, Delete Completed, Options, Scheduler, Open file, Open folder.
  Add-URL dialog pre-fills from the clipboard. Per-download worker
  threads; progress is pushed to the UI thread via `PostMessage`. Double-
  click opens the file; right-click opens a context menu. System-tray
  icon with minimize-to-tray and balloon notifications. Clipboard
  watcher prompts when a fresh URL appears. Command-line argument is
  auto-queued so `matata://URL` links and the Send-to shortcut work.
- **OLE drop target**: drag URL text or files onto the main window and
  they're queued immediately (multi-line URL paste supported; dropped
  files are queued as `file://` URLs, useful for local `.m3u8`/`.mpd`).
- **Options dialog**: default download folder, connections per download,
  bandwidth cap (KB/sec), per-type subfolder routing, checksum verify,
  clipboard-watch toggle. Apply/Cancel. Saves to `HKCU\Software\matata\Gui`.
- **Scheduler dialog**: global `Start queue at` + `Stop queue at` (HH:MM
  24-hour, with enable-checkboxes each side). Applied per-item when
  added: start-at delays spawn of the worker thread; stop-at aborts
  running transfers cleanly so they resume on next launch.
- **Persistent GUI settings**: window placement, maximized state, column
  widths (8), tree-pane width, and every Options/Scheduler value are
  written to `HKCU\Software\matata\Gui` on graceful shutdown and restored
  on the next launch. No INI file — pure registry, HKCU, no admin.
- **FTP** (plain, RFC 959) — a hand-rolled Winsock client. URL parser
  accepts `ftp://[user[:pass]@]host[:port]/path`; anonymous auth used
  when creds are omitted. Implements `USER`/`PASS`/`TYPE I`/`SIZE`/
  `EPSV` (fallback to `PASV`)/`REST` (resume)/`RETR`. Single stream but
  resumable via `.mtpart`; cancellation is cooperative. Wired into both
  the CLI and the GUI — the usual progress bar, status bar, and Save-as
  logic work. Verified against ftp.gnu.org: 2.75 KiB README and 1.11 MiB
  tarball pulled cleanly, both valid at the byte level.
- **FTPS** (explicit TLS, RFC 4217) — `AUTH TLS` on the control channel,
  then `PBSZ 0` + `PROT P` so the data channel is also encrypted. TLS
  via Windows SChannel (`Secur32.lib`), with a process-wide client
  credential so SChannel can cache sessions — required by strict servers
  that enforce data-channel session resumption (e.g. rebex.net,
  `require_ssl_reuse=YES` vsftpd). Verified against
  `ftps://demo:password@test.rebex.net/readme.txt`: 379 bytes pulled.
- **BitTorrent foundation**: honest MVP. [bencode.hpp](include/matata/bencode.hpp)
  is a complete bencode parser (int/string/list/dict with re-encode), and
  [torrent.hpp](include/matata/torrent.hpp) parses both `.torrent` files
  (with proper SHA-1 of the raw info-dict bytes via BCrypt, multi-file
  support, `announce` + `announce-list`) and `magnet:?xt=urn:btih:<hex>`
  URIs. What's intentionally missing: tracker protocol, peer wire
  protocol, piece verification — a real client is thousands of LOC and
  IDM itself doesn't do torrents. The parser gives us everything we'd
  need to build one.
- **Installer** — [installer/matata.iss](installer/matata.iss), an Inno
  Setup 6 script. Bundles all four build artifacts + the extension
  tree, registers the `matata:` URL scheme under HKCU, drops a SendTo
  shortcut, and runs `regsvr32` on `matata_shell.dll` for Explorer
  integration. Compile with:
  ```
  "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\matata.iss
  ```
  Output lands in `build\installer\matata-<version>-setup.exe`.
- **Auto-update client** — [updater.hpp](include/matata/updater.hpp)
  fetches a JSON manifest (`{"version":"...","url":"...","notes":"..."}`),
  compares against the baked-in version via semver, and reports whether
  an upgrade is available. CLI: `matata --check-update [URL]`. The
  heavy lifting (actually downloading and launching the new installer)
  is a trivial `ShellExecute` once a real update server is hosted.
- **COM shell extension DLL** (`matata_shell.dll`): in-proc `IShellExtInit`
  + `IContextMenu` handler that adds a **"Download with matata"** entry
  to the right-click menu on any file, any directory, and the Explorer
  background. Registered via HKCU (no admin) — `regsvr32 matata_shell.dll`
  installs, `regsvr32 /u` removes. CLSID `{7E1C8F52-4B1A-4A3D-9C2F-4C5E6D7B8A90}`.
- **Shell integration**: `install-shell.bat` registers a `matata:` URL
  scheme (HKCU — no admin) so `matata://https://...` links anywhere on
  the system launch `matata-gui.exe` with the URL; also drops a
  `Send to → matata` shortcut in `%APPDATA%\...\SendTo`.

### Build

```
build.bat          :: release
build.bat debug    :: debug with /Zi
build.bat clean    :: wipe build/
```

Requires **Visual Studio 2022 Build Tools** with the C++ toolset and
Windows 10/11 SDK. The script shells out to `vcvars64.bat`.

### Use

**GUI:** launch `build\matata-gui.exe` (no args), or
`matata-gui.exe <URL>` to auto-queue. See the clipboard watcher, tray
icon, and Add-URL dialog.

**CLI:**

```
matata <url> [url ...] [options]
matata --help
```

**Output**
- `-o NAME` — output filename (single-URL only)
- `-d DIR` — output directory (default: current)
- `--categorize` — route each file into a subfolder by extension

**Connections**
- `-n N` — connections per download (default 8, max 32)
- `-j N` — max concurrent downloads (default 3)
- `-J N` — max total HTTP connections across the queue (default 16)

**Headers**
- `--cookie STR`, `--referer URL`, `--user-agent STR`
- `-H 'K: V'` — arbitrary header, repeatable

**Auth**
- `--user USER:PASS` — HTTP Basic auth
- `--bearer TOKEN` — HTTP Bearer auth
- `--netrc` — look up creds in `%USERPROFILE%\_netrc`
- `--netrc-file PATH` — look up creds in a specific netrc file

**Throttling**
- `--limit-rate BYTES` — global bandwidth cap, suffixes `k/m/g` accepted

**Verification**
- `--no-verify-checksum` — skip the digest check even if server advertised one

**Queue**
- `--queue FILE` — persist/resume queue from FILE
- `--start-at TIME` — hold queue until TIME (`HH:MM` or `YYYY-MM-DD HH:MM`)
- `--stop-at TIME` — stop dispatching new items after TIME
- `--shutdown-on-done` — Windows shutdown after the whole queue completes

**Video (HLS)**
- `--hls` / `--no-hls` — force or disable HLS mode (default: auto-detect `.m3u8`)
- `--quality Q` — `best` / `worst` / `720p` / `1080p` / `<height>`
- `--ffmpeg PATH` — remux the concatenated `.ts` into `.mp4`
- `--video-parallel N` — parallel segment downloads (default: 8)

**Diagnostics**
- `-v` / `--verbose` — probe diagnostic (shows digest, HLS variant pick)

### Browser integration (Chrome / Edge)

After `build.bat` has produced `build\matata.exe` AND `build\matata-host.exe`:

1. Open `chrome://extensions` (or `edge://extensions`), enable **Developer
   mode**, click **Load unpacked**, and pick the `extension\` folder.
2. Copy the 32-character extension ID that Chrome assigns it.
3. From a cmd shell in the project root, run:

   ```
   install-extension.bat <EXTENSION_ID>
   ```

   This writes `native-host\com.matata.host.json` with your ID baked in and
   registers it under `HKCU\Software\{Google\Chrome,Microsoft\Edge,Chromium}\
   NativeMessagingHosts\com.matata.host`.
4. Fully quit and reopen Chrome / Edge so the new host registration is
   picked up.
5. Click the extension's toolbar icon → **ping native host** to confirm
   the handshake works.

From then on, any download whose extension matches the configured list (and
whose size is ≥ `minSize`) is intercepted: the browser transfer is cancelled
and `matata.exe` starts, with `Referer:`, `Cookie:`, and `User-Agent:`
headers forwarded from the browser.

To remove: `uninstall-extension.bat`.

### Publishing the browser extension (Chrome Web Store + Edge Add-ons)

The extension in `extension/` is ready to publish. Workflow:

**0. Push to GitHub + enable Pages** (one-time)

The Web Store listing needs a hosted privacy policy, homepage, and
support page. All three are already written in `docs/` and ready to
host on GitHub Pages:

```
cd C:\Users\fame\Documents\bin\matata
git init
git add .
git commit -m "matata v0.7.1"
git branch -M main
git remote add origin https://github.com/morris2016/matata-idm.git
git push -u origin main
```

Then in GitHub: **Settings → Pages → Source: `main` / `/docs` → Save.**
First build takes ~60s. Resulting URLs:

- `https://morris2016.github.io/matata-idm/`        (Homepage)
- `https://morris2016.github.io/matata-idm/privacy` (Privacy policy)
- `https://morris2016.github.io/matata-idm/support` (Support)

**1. Produce the upload bundle**

```
powershell -ExecutionPolicy Bypass -File pack-extension.ps1
```

Validates `manifest.json`, verifies all referenced icon files exist,
and writes `build\matata-extension-<version>.zip`.

**2. Submit to the stores**

- **Chrome Web Store** — https://chrome.google.com/webstore/devconsole
- **Microsoft Edge Add-ons** — https://partner.microsoft.com/dashboard/microsoftedge
  (same zip, free submission, reaches Edge users cleanly)
- **Firefox AMO** — https://addons.mozilla.org/developers/ (the
  `browser_specific_settings.gecko.id` in manifest.json is already set)

Paste copy from [extension/STORE_LISTING.md](extension/STORE_LISTING.md)
into each listing form. You'll need:

- A publisher **Address** filled in on your developer Account page
  (the $5 developer fee is not enough — Google blocks submissions
  without an address).
- The three GitHub Pages URLs above (privacy / homepage / support).
- 1-5 screenshots (1280x800 PNGs), a 440x280 promo tile, and the
  128x128 icon (already at `extension/icons/icon-128.png`).

3. **After Google approves**, Chrome assigns a **permanent extension
   ID**. Grab it from `chrome://extensions` (or the devdash URL) and
   run:

   ```
   install-extension.bat <PUBLISHED_EXTENSION_ID>
   ```

   This re-writes `native-host\com.matata.host.json` with the new ID in
   `allowed_origins` and points the Chrome/Edge/Chromium registry at
   it. Users who install via your installer will use this same
   registration — so ship the updated `com.matata.host.json` in the
   installer once you have the ID.

4. **Subsequent releases**: bump `"version"` in
   `extension/manifest.json`, re-run `pack-extension.ps1`, upload the
   new zip through the dashboard (same listing). The extension ID
   never changes.

5. **Optional: CI uploads via the Publish API.** Your dashboard has a
   GCP service account attached already (dev dashboard → Account →
   Service account). With it you can POST the zip via the Chrome Web
   Store API from CI — see the `Publishing via the API` section at the
   bottom of STORE_LISTING.md for the exact endpoints.

### Shell extension (right-click "Download with matata")

Run these from an elevated / non-elevated cmd (it uses HKCU so no admin):

```
regsvr32 build\matata_shell.dll       :: register
regsvr32 /u build\matata_shell.dll    :: remove
```

After registration, restart Explorer (`taskkill /IM explorer.exe /F && start explorer`)
or log out / back in so the context-menu shell extension is picked up.

## Roadmap to IDM parity

The v0.1 engine is deliberately narrow — one URL in, one file out. Everything
below is additive.

### v0.2 — engine quality

- [x] **Dynamic segment stealing.**
- [x] **HTTP/2** enabled via `WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL`.
- [x] **Verbose mode** (`-v`).
- [x] **Bandwidth cap** (`--limit-rate`).
- [x] **Cookie / referer / user-agent / arbitrary headers.**
- [x] **Retry/backoff** with per-segment progress reset.
- [x] **Checksum verification** when the server advertises `Digest:` or
      `Content-MD5:` (MD5/SHA-1/SHA-256/SHA-384/SHA-512 via BCrypt; opt out
      with `--no-verify-checksum`).
- [x] **Auth**: `--user USER:PASS` (Basic), `--bearer TOKEN`, `--netrc`,
      `--netrc-file PATH`.

### v0.3 — queue & scheduler

- [x] **Multi-download queue** with per-download (`-j`) and total-connection
      (`-J`) caps.
- [x] **Plain-text state store** (`--queue FILE`) for persistent queue.
- [x] **Categories**: extension-based subfolder routing (`--categorize`).
- [x] **Shutdown on completion** (`--shutdown-on-done`).
- [x] **Time-windowed scheduler** (`--start-at`, `--stop-at`) —
      `HH:MM` (rolls forward if past) or `YYYY-MM-DD HH:MM`.

### v0.4 — browser integration (the IDM moat)

- [x] **Chrome / Edge WebExtension (Manifest V3)** hooks
      `chrome.downloads.onCreated`: cancels matching transfers and hands
      URL + cookies + referer to the native host.
- [x] **Native messaging host** (`matata-host.exe`) with length-prefixed
      JSON stdio protocol. Spawns `matata.exe` with a piped stdout and
      forwards each JSON progress line back to the extension.
- [x] **Install / uninstall scripts** that register `com.matata.host` with
      Chrome, Edge, Chromium, and Firefox (HKCU).
- [x] **Firefox WebExtension**: same contract; `browser_specific_settings.gecko.id`
      in the manifest + Mozilla-specific `allowed_extensions` JSON.
- [x] **Bi-directional progress**: extension uses a persistent `connectNative`
      port; host forwards per-job JSON events; popup shows a live queue.

### v0.5 — video grabber

- [x] **HLS (`.m3u8`)** master + media playlist parser; variant picker
      (`--quality`); parallel segment download; byte-wise `.ts` concat;
      optional `--ffmpeg` remux to `.mp4`.
- [x] **HLS AES-128** decryption via BCrypt AES-CBC (key fetch + cache,
      explicit or sequence-derived IV).
- [x] **HLS live playlists** (poll-and-append until `#EXT-X-ENDLIST`).
- [x] **DASH (`.mpd`)**: minimal XML parser, SegmentTemplate ($Number$ /
      $Time$ / $RepresentationID$ / $Bandwidth$), SegmentList, SegmentTimeline,
      cascaded `<BaseURL>`s. Optional `--ffmpeg` audio+video mux.
- [x] **Browser-side media sniffer** — extension detects HLS/DASH URLs via
      `chrome.webRequest.onBeforeRequest` and offers them in the popup.

### v0.6 — Windows UX

- [x] **Win32 GUI** (pure Win32 / C++17; common controls): main window,
      toolbar, ListView, status bar, menu, Add-URL dialog, context menus.
- [x] **System tray icon** with minimize-to-tray, restore-on-click, and
      balloon notifications on completion / errors.
- [x] **Clipboard URL watcher** prompts the user when a fresh HTTP(S) URL
      is copied.
- [x] **`matata:` URL scheme** registration (HKCU) and a `Send to → matata`
      shortcut via `install-shell.bat`.
- [x] **Command-line URL forwarding** — `matata-gui.exe <URL>` auto-queues.
- [x] **Full COM shell extension** (`IShellExtInit` + `IContextMenu`) —
      `matata_shell.dll`, self-registering via `regsvr32`, adds "Download
      with matata" to the right-click menu on files, folders, and the
      Explorer background.
- [x] **OLE drop-target** on the main window — drop URL text or files
      and they're queued (multi-URL paste splits on newlines; files become
      `file://` URLs).
- [x] **IDM-style layout** — category tree + 8-column ListView + per-row
      file icons (matching the reference screenshot).
- [x] **Real Options and Scheduler dialogs** (no more placeholder
      MessageBox); all settings persist across runs via HKCU registry.

### v0.7 — protocols beyond HTTP + packaging

- [x] **FTP** (plain, RFC 959): handwritten Winsock client with EPSV/PASV
      fallback and REST-based resume. `ftp://user:pass@host/path` works.
- [x] **FTPS** (explicit TLS via `AUTH TLS` + SChannel + session cache).
- [x] **BitTorrent foundation**: bencode parser, `.torrent` parser (with
      SHA-1 info-hash via BCrypt), `magnet:?xt=urn:btih:<hex>` parser.
      Tracker + peer protocol are beyond the session scope.
- [x] **Installer** (Inno Setup `.iss`).
- [x] **Auto-update client** (`--check-update URL`). A real update
      server is required to make it meaningful end-to-end.
- [ ] **SFTP** — needs linking libssh2. Concrete path below.
- [ ] **Full BitTorrent engine** — tracker HTTP/UDP, peer wire protocol,
      per-piece SHA-1 verify. Biggest remaining gap; most pragmatic
      route is linking libtorrent-rasterbar.

#### How to finish SFTP

SFTP runs over SSH. Windows doesn't ship an SSH stack; the usual
solution is [libssh2](https://libssh2.org/). Concrete plan:

1. Obtain / build libssh2 (OpenSSL or WinCNG crypto backend; WinCNG
   avoids shipping OpenSSL).
2. Drop `libssh2.lib` + `libssh2.dll` next to matata's other binaries.
3. Add a new `SftpClient` alongside `FtpClient` that opens an SSH
   session via `libssh2_session_init`, authenticates (password or
   pubkey), and uses `libssh2_sftp_*` for file transfer.
4. Wire `sftp://` into the URL parser's allowlist, add the scheme
   detector in CLI + GUI dispatch, and reuse the `FtpDownloader`-style
   wrapper pattern (progress callback, `.mtpart` resume).

Rough estimate: one focused session given prebuilt libssh2 binaries.

#### How to finish BitTorrent

The honest options:

- **Link libtorrent-rasterbar** (1,000+ KLOC, depends on Boost). Most
  production clients use this. Packaging overhead is real.
- **Hand-write an MVP** (~3,000 LOC): tracker query (HTTP or UDP),
  peer wire protocol, piece picker, SHA-1 verification, file
  allocation. No DHT, no encryption, no PEX. Takes several sessions.

The bencode / `.torrent` / magnet infrastructure already here is reusable
in either path — tracker requests need the info-hash; peer handshake
needs it too.

### Things IDM does that matata deliberately won't

- Kernel-mode Windows Filtering Platform / TDI drivers
  (`idmwfp64.sys`, `idmtdi64.sys`). Modern browsers + native messaging make
  kernel interception unnecessary; the signing + maintenance cost isn't worth
  it.
- Old Internet Explorer integration (`IEMonitor.exe`, IE COM BHO). IE is dead.
- MFC and ATL. Pure Win32 + C++17.
- Per-year licensing. matata is free.

## Project layout

```
matata/
├── build.bat                 MSVC build wrapper (produces three exes)
├── install-extension.bat     registers native host for Chrome + Edge + Firefox
├── uninstall-extension.bat
├── install-shell.bat         registers matata: URL scheme + SendTo shortcut
├── uninstall-shell.bat
├── shell-ext/
│   ├── matata_shell.cpp      COM shell extension (IShellExtInit + IContextMenu)
│   └── matata_shell.def      DLL export table
├── installer/
│   └── matata.iss            Inno Setup 6 installer script
├── include/matata/
│   ├── url.hpp               URL parser + Url struct
│   ├── http.hpp              WinHTTP client (probe / ranged GET + HTTP/2)
│   ├── bandwidth.hpp         Token-bucket RateLimiter
│   ├── base64.hpp            base64 encode / decode
│   ├── digest.hpp            MD5/SHA-* via BCrypt; Digest + Content-MD5 parsers
│   ├── auth.hpp              Basic / Bearer; netrc lookup
│   ├── segments.hpp          Segment + DownloadMeta (atomics, sidecar)
│   ├── categories.hpp        Extension -> subfolder routing
│   ├── downloader.hpp        Multi-threaded segmented downloader (+stealing)
│   ├── queue.hpp             Multi-download queue + scheduler + persistence
│   ├── aes.hpp               AES-128-CBC wrapper (BCrypt) + IV helpers
│   ├── hls.hpp               HLS manifest parser (master + media + keys)
│   ├── dash.hpp              DASH manifest parser (minimal XML + templates)
│   ├── video.hpp             VideoGrabber orchestrator (HLS + DASH paths)
│   ├── ftp.hpp               FTP/FTPS Winsock client (EPSV/PASV + REST + AUTH TLS)
│   ├── ftp_downloader.hpp    FtpDownloader (Downloader-compatible shell)
│   ├── tls.hpp               SChannel TLS wrapper (shared-credential session cache)
│   ├── bencode.hpp           bencode parser (BitTorrent foundation)
│   ├── torrent.hpp           .torrent + magnet URI parser
│   └── updater.hpp           auto-update client (JSON manifest)
├── src/                      (14 lib .cpp + main.cpp + gui_main.cpp)
├── native-host/
│   ├── host.cpp              matata-host.exe — Native Messaging stdio loop
│   └── com.matata.host.json  generated by install-extension.bat
├── extension/
│   ├── manifest.json         Manifest V3
│   ├── background.js         service worker: intercepts + dispatches
│   ├── popup.html            settings + manual URL entry + diagnostics
│   └── popup.js
└── build/                    (gitignored; build output)
```

## File format

A partially-downloaded file is two files next to each other:

- `target.mtpart` — the body, preallocated to final size, written at
  per-segment offsets.
- `target.mtmeta` — plain-text key=value sidecar. Format is intentionally
  human-readable and easy to inspect with any editor.

On success: `.mtpart` is renamed to `target`, `.mtmeta` is deleted.

## License

TBD — plan is permissive (MIT / Apache-2.0). Not yet open-source pending
initial polish.
