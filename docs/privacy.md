---
title: matata — Privacy Policy
description: What the matata download helper does and does not do with your data.
---

# Privacy Policy

_Last updated: 2026-04-24_

This policy covers the **matata download helper** browser extension and
the **matata** native application. The extension and the app are
developed by the same author and always function as a pair.

## What data the extension sees

The extension runs inside your browser. The set of data it can observe
is limited to what the following Chrome / WebExtension permissions
require:

| Permission | Why it's declared |
|---|---|
| `downloads`        | Read URL, filename, and referrer of downloads you trigger, so the matata app can take them over. |
| `cookies`          | Read cookies for the origin of a download so authenticated files (session-gated downloads, logged-in content) still work after matata fetches them. |
| `nativeMessaging`  | Communicate with the locally-installed matata app via the Native Messaging channel. |
| `storage`          | Persist your own settings (extension filter, minimum size, clipboard-watch toggle, etc.) between browser sessions. |
| `webRequest`       | Observe — **not block** — `.m3u8` and `.mpd` requests so the popup can list streams you might want to grab. |
| `tabs`             | Identify which tab is active so the detected-streams list is filtered to that tab. |
| `host_permissions: <all_urls>` | The three permissions above need to work on arbitrary origins because you can download from arbitrary origins. |

## What data leaves your computer

**None of it goes to a server we control.** There is no analytics, no
telemetry, no crash reporting, no account, and no login.

The only place data goes is:

1. **To the locally-installed matata app** (via Native Messaging, a
   local stdio pipe — nothing touches the network). The message
   contains the URL, the user-agent, and optionally a referer and
   cookies for that origin, so matata can issue the correct HTTP
   request itself.

2. **To the server hosting your download.** This is a normal HTTP
   request from your own machine, the same one your browser would have
   made — just performed by matata with better segmentation and
   resume.

## What data the app stores on disk

- Downloaded files go wherever you tell them to.
- A `.mtpart` sidecar next to each in-progress download (body +
  metadata used for resume).
- Your preferences under
  `HKEY_CURRENT_USER\Software\matata\Gui` (column widths, window
  placement, default output folder, bandwidth cap, etc.).
- A plain-text queue state file if you explicitly opt in via
  `--queue FILE` on the CLI.

## What we never do

- We never upload the list of URLs you download, the files themselves,
  your cookies, or anything derived from them.
- We never phone home to check for updates unless you explicitly run
  `matata --check-update` (and even then, only against a URL you
  control).
- We never install, launch, or update any other software.
- We never sell, share, or transfer data to third parties.

## Open-source verification

The extension and app are open source. You can audit every line at
[github.com/morris2016/matata-idm](https://github.com/morris2016/matata-idm).
The build is reproducible: `build.bat` produces exactly the four
binaries shipped in the installer.

## Contact

Issues, questions, or data-removal requests:
[github.com/morris2016/matata-idm/issues](https://github.com/morris2016/matata-idm/issues)
or by email at **siagmoo26@gmail.com**.

## Changes to this policy

If this policy changes, the `Last updated` date above will change with
it. The canonical version lives at the URL you're reading now.
