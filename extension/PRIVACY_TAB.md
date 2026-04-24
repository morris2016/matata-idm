# Privacy tab — paste each section into its labelled field

The Privacy tab (left sidebar in the Chrome Web Store dev dashboard)
has several mandatory fields. Fill them all; Submit-for-review stays
disabled until every one is populated.

---

## Single purpose (required, free-form text)

> matata download helper intercepts downloads triggered in the browser
> and hands them to the matata native application, which performs the
> actual transfer with parallel connections, resume, and a queue. The
> extension does not download anything by itself — it is a bridge
> between the browser and the local app.

---

## Permission justifications (one per declared permission)

**`downloads`**

> Read URL, filename, MIME type, and referrer for each download the
> user initiates, so matata can take over the transfer and optionally
> cancel the browser's own copy. No downloads are inspected or stored
> beyond forwarding their metadata to the local matata application via
> Native Messaging.

**`storage`**

> Persist per-user settings between browser sessions: the list of file
> extensions to intercept, the minimum size threshold, the output
> folder hint, and the clipboard-watcher toggle. All values stay on
> the user's device.

**`cookies`**

> Read cookies for the origin of a download so authenticated or
> session-gated files (logged-in downloads, signed URLs) still succeed
> when matata fetches the URL itself. Cookies are forwarded only to
> the locally-installed matata application via Native Messaging and
> are never sent to any network endpoint the extension controls.

**`nativeMessaging`**

> Communicate with the locally-installed matata native host
> (matata-host.exe) over the Native Messaging stdio pipe. This is the
> sole way the extension talks to the app. The permission does not
> involve network traffic.

**`webRequest`**

> Observe network requests (non-blocking, Manifest V3 observation
> mode) to detect HLS (.m3u8) and DASH (.mpd) manifest URLs. Detected
> URLs are listed in the popup so the user can grab a stream with one
> click. The extension never modifies, blocks, or redirects requests.

**`tabs`**

> Identify which tab is currently active so the sniffed-streams list
> shown in the popup is filtered to that tab. No other tab data is
> read or stored.

**Host permissions: `<all_urls>`**

> Required because the user can download from any origin. The
> extension reads cookies and classifies download URLs from whatever
> site initiated the download; it does not read page content,
> DOM, or execute scripts in any page.

---

## Data usage (the checkboxes / dropdowns on the Privacy tab)

Tick EXACTLY these — everything else is **No**:

- **I do not collect or transmit user data**: YES (the whole point)
- Remote code (e.g., eval, injected scripts from remote servers): NO
- Payments: NO
- Ads or third-party analytics: NO
- Authentication data handling: NO
- Personally identifiable info collected: NO
- Health info: NO
- Financial info: NO
- User communications: NO
- Location: NO
- Web history: NO
- User activity (clicks/mouse/keyboard): NO
- Website content (DOM/page text): NO

Certify both statements at the bottom of the tab:

- **I do not sell or transfer user data to third parties...**: YES
- **I do not use or transfer user data for purposes unrelated to my
  item's single purpose...**: YES
- **I do not use or transfer user data to determine creditworthiness
  or for lending purposes...**: YES

---

## Privacy policy URL (required because of `<all_urls>`)

> https://morris2016.github.io/matata-idm/privacy
