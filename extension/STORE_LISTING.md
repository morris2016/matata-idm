# Chrome Web Store listing copy

Paste these straight into the developer dashboard submission form.

---

## Name
`matata download helper`

## Summary (132 char max)
`Fast segmented downloads, HLS/DASH capture, and a clean queue — routed to the native matata app.`

## Category
`Workflow & Planning`
(Chrome Web Store's current taxonomy — replaces the older "Productivity".
If the dashboard still shows "Productivity" in some regions, that's the
same bucket.)

## Language
`English`

## Description

```
matata is a native Windows download manager. This extension is the
browser-side half: it intercepts downloads from Chrome and hands them to
the matata app, which does the actual transfer with parallel
connections, resume on failure, and a proper queue.

Features
  • Intercepts matching downloads (by file extension and size) and runs
    them through matata — up to 32 parallel connections per file, with
    full resume via a .mtpart sidecar.
  • Detects HLS (.m3u8) and DASH (.mpd) streams on the page and lets
    you grab them from the popup with one click.
  • Forwards request headers the matata app needs (Cookie, Referer,
    User-Agent) so authenticated downloads work.
  • Live "Active downloads" list in the popup, updating as the matata
    app reports progress back over Native Messaging.
  • Works alongside the existing browser downloads manager; bypass
    matata for any specific download by holding Alt when you click.

Requires the matata app installed separately. Get it at
<your website>. The extension by itself is just a bridge — it does
not download anything without the app.

Source:    <github link>
License:   <license>
Privacy:   <privacy policy URL>
```

(Fill in the bracketed items before submitting.)

---

## Permission justifications (required by Google review)

Each permission below appears in the listing; write one sentence per item.

- **`downloads`** — Read download metadata (URL, filename, referrer) so
  matata can take over the transfer and optionally cancel the browser's
  own copy.
- **`storage`** — Persist per-user settings (min size, extension filter,
  output folder, clipboard-watch toggle) between sessions.
- **`cookies`** — Read cookies for the target origin so authenticated
  downloads (session-gated files) still work when matata fetches them.
- **`nativeMessaging`** — Communicate with the locally-installed matata
  native host. No network traffic goes through this permission.
- **`webRequest`** — Observe (not block) HLS/DASH manifest requests so
  we can surface them in the popup. MV3 observation-only mode.
- **`tabs`** — Know which tab is active so the sniffed-streams list is
  filtered to the current tab.
- **`host_permissions: <all_urls>`** — Read cookies and classify
  download URLs from any origin. Required because users download from
  arbitrary sites.

## Screenshots (1280x800 or 640x400, up to 5 PNG/JPEG)

Screenshots to capture:
1. **Popup — active downloads**  — a download in progress showing the
   green progress bar and live speed.
2. **Popup — streams on this tab** — `.m3u8` detected on a media page.
3. **Popup — manual URL entry** — pasting a URL into the bottom field.
4. **Settings section**  — the "Interceptor" and "Diagnostics" blocks.
5. **Matata app main window** (not strictly required but strongly
   helps: shows what downloading ones do once handed off).

## Promo tile (440x280, required)

A clean tile with the matata icon + the tagline "IDM-style, open source,
free." Against a solid colour background.

## Privacy policy URL (required when an extension uses `<all_urls>`)

Already written and ready to host: [docs/privacy.md](../docs/privacy.md).

Once you push the repo (`github.com/morris2016/matata-idm`) and enable
GitHub Pages with source `main` / folder `/docs`, the canonical URLs
will be:

| Listing field       | URL |
|---------------------|-----|
| **Homepage URL**       | `https://morris2016.github.io/matata-idm/` |
| **Privacy policy URL** | `https://morris2016.github.io/matata-idm/privacy` |
| **Support URL**        | `https://morris2016.github.io/matata-idm/support` |

Paste these into the Web Store listing. Same three URLs work for the
Microsoft Edge Add-ons listing too.

---

## Before you hit Submit

1. **Account → Address** in the developer dashboard is required for
   publishers who've paid the $5 fee. Not setting it = rejection.
2. Install the zip produced by `pack-extension.ps1` in dev mode first
   to smoke-test.
3. After first publish, Google assigns a permanent extension ID.
   **Write it down** — you need it to register the native host on every
   install via `install-extension.bat <ID>`.
4. Subsequent releases: bump `"version"` in `manifest.json`, re-run
   `pack-extension.ps1`, upload as a new version in the existing
   listing.

## Publishing via the API (optional, nicer for CI)

Your service account `matata@atlantean-force-494312-d3.iam.gserviceaccount.com`
can drive the Chrome Web Store Publish API. Flow:

1. Enable the "Chrome Web Store API" in the GCP project.
2. `gcloud auth application-default login` once locally, then use the
   service-account key file.
3. `POST https://www.googleapis.com/upload/chromewebstore/v1.1/items/<appId>`
   with the zip as the body to push a new version.
4. `POST https://www.googleapis.com/chromewebstore/v1.1/items/<appId>/publish`
   to promote it.

See https://developer.chrome.com/docs/webstore/using-api for the full
reference.
