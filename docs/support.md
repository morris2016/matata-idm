---
title: matata — Support
description: How to get help with matata.
---

# Support

## Report a bug / request a feature

File an issue at
[github.com/morris2016/matata-idm/issues](https://github.com/morris2016/matata-idm/issues).

If the issue is a crash, attach:

- Your matata version (`matata --version`, or `Help → About` in the GUI).
- Windows version (`winver`).
- The URL you were downloading (redact signed-URL tokens if any).
- If possible, output from `matata <URL> -v` on the command line, or
  the contents of the `.mtmeta` sidecar next to the partial file.

## Email

**siagmoo26@gmail.com**

Email is best for:

- Disclosing security issues privately.
- Requests to remove or correct data we hold (for the record: we don't
  hold any — see [privacy](./privacy)).

## Frequently-asked

### The browser extension can't reach the native host

1. Fully quit the browser and reopen it (close all windows, not just
   the current tab).
2. Run `install-extension.bat <YOUR_EXTENSION_ID>` from the matata
   install directory. The ID is visible in `chrome://extensions` with
   Developer mode on.
3. Click the extension's toolbar icon → **ping native host**. A
   green "host ok" confirms the channel.

### Downloads fail with "probe failed: HTTP 403"

The URL you gave matata has probably expired. Signed download URLs
from many CDN-served sites (filehorse, Google Drive, MediaFire, some
S3 buckets) include a short-lived signature. Copy a fresh URL from the
site and retry.

### FTPS fails with "425 Cannot secure data connection"

You're hitting a server that requires TLS session resumption between
control and data channels. matata does this correctly out of the box —
if it still fails, you're likely on a very old Windows version where
SChannel's TLS 1.2 session cache is disabled.

### How do I get rid of matata entirely?

Run the uninstaller (Start → Apps → matata → Uninstall) or, if you
installed via the manual route:

```
uninstall-extension.bat
uninstall-shell.bat
regsvr32 /u build\matata_shell.dll
```

and delete the install folder.
