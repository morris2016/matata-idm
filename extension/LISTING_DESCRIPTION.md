# Paste this verbatim into the "Description" field

(Replaces the contents of STORE_LISTING.md that you copied in — the
dashboard wants only the user-facing copy, not my instructions.)

---

matata is a native Windows download manager. This extension is the
browser-side half: it intercepts downloads from Chrome and hands them
to the matata app, which does the actual transfer with parallel
connections, resume on failure, and a proper queue.

Features

  - Intercepts matching downloads (by file extension and size) and runs
    them through matata — up to 32 parallel connections per file, with
    full resume via a .mtpart sidecar.
  - Detects HLS (.m3u8) and DASH (.mpd) streams on the page and lets
    you grab them from the popup with one click.
  - Forwards request headers the matata app needs (Cookie, Referer,
    User-Agent) so authenticated downloads work.
  - Live "Active downloads" list in the popup, updating as the matata
    app reports progress over Native Messaging.
  - Works alongside the existing browser downloads manager; bypass
    matata for any specific download by holding Alt when you click.

Requires the matata app installed separately:
https://morris2016.github.io/matata-idm/

The extension by itself is just a bridge — it does not download
anything without the app.

Source code:   https://github.com/morris2016/matata-idm
License:       MIT
Privacy:       https://morris2016.github.io/matata-idm/privacy
