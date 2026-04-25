const DEFAULT_EXTS = [
  "zip","rar","7z","tar","gz","bz2","xz","iso",
  "mp4","mkv","avi","mov","webm","flv","mpg","mpeg","m4v","ts",
  "mp3","flac","wav","m4a","aac","ogg","opus",
  "exe","msi","apk","dmg","pkg","deb","rpm",
  "pdf","epub","mobi","bin","img","vhd","vmdk"
];
const DEFAULTS = {
  enabled: true,
  minSize: 1048576,
  extensions: DEFAULT_EXTS,
  outDir: "",
  confirmEach: true,
  skipExts: []
};

const $ = (id) => document.getElementById(id);

async function load() {
  const stored = await chrome.storage.local.get(["settings"]);
  const s = { ...DEFAULTS, ...(stored.settings || {}) };
  $("enabled").checked = !!s.enabled;
  $("confirmEach").checked = s.confirmEach !== false;
  $("minSize").value = s.minSize;
  $("extensions").value = (s.extensions || []).join(", ");
  $("outDir").value = s.outDir || "";
  $("skipExts").value = (s.skipExts || []).join(", ");
}

async function save() {
  const extList = $("extensions").value
    .split(/[,\s]+/).map(x => x.trim().toLowerCase()).filter(Boolean);
  const skipList = $("skipExts").value
    .split(/[,\s]+/).map(x => x.trim().toLowerCase().replace(/^\./, "")).filter(Boolean);
  const settings = {
    enabled:     $("enabled").checked,
    confirmEach: $("confirmEach").checked,
    minSize:     Math.max(0, parseInt($("minSize").value, 10) || 0),
    extensions:  extList,
    outDir:      $("outDir").value.trim(),
    skipExts:    skipList
  };
  await chrome.storage.local.set({ settings });
  const s = $("saveStatus");
  s.textContent = "saved.";
  setTimeout(() => (s.textContent = ""), 1500);
}

function sendBg(msg) {
  return new Promise((resolve) => chrome.runtime.sendMessage(msg, resolve));
}

async function sendManual() {
  const url = $("mUrl").value.trim();
  if (!url) return;
  const outDir = $("outDir").value.trim();
  const r = await sendBg({ action: "manual-download", url, filename: $("mName").value.trim(), outDir });
  const el = $("status");
  el.className = r && r.ok ? "ok" : "err";
  el.textContent = r && r.ok ? "queued." : ("error: " + (r && r.err || "no reply"));
}

async function ping() {
  const el = $("status");
  el.textContent = "pinging...";
  el.className = "muted";
  const r = await sendBg({ action: "ping-host" });
  if (r && r.ok && r.r && r.r.ok === "true") {
    el.className = "ok";
    el.textContent = "host ok (v" + (r.r.version || "?") + ")";
  } else {
    el.className = "err";
    el.textContent = "host unreachable: " + (r && r.err || "bad response");
  }
}

function fmtBytes(n) {
  if (!n || n < 0) return "?";
  const u = ["B","KiB","MiB","GiB","TiB"];
  let i = 0, v = n;
  while (v >= 1024 && i < u.length - 1) { v /= 1024; ++i; }
  return (i === 0 ? v : v.toFixed(2)) + " " + u[i];
}

function jobSummary(j) {
  const p = j.latest || {};
  if (p.kind === "video") {
    const pct = (p.percent || 0).toFixed(1);
    return `video  ${p.done||0}/${p.total||0} segs  ${fmtBytes(p.bytes)}  @ ${fmtBytes(p.rate)}/s`;
  }
  if (p.kind === "http") {
    const pct = (p.percent || 0).toFixed(1);
    return `http  ${fmtBytes(p.downloaded)} / ${fmtBytes(p.total)}  @ ${fmtBytes(p.rate)}/s  (${p.conns||0} conn)`;
  }
  return j.status || "";
}

function jobPercent(j) {
  return Math.max(0, Math.min(100, (j.latest && j.latest.percent) || 0));
}

async function refreshJobs() {
  const container = $("jobList");
  const r = await sendBg({ action: "list-jobs" });
  const list = (r && r.ok) ? (r.jobs || []) : [];
  if (list.length === 0) {
    container.textContent = "(none)";
    container.className = "muted";
    return;
  }
  container.className = "";
  container.innerHTML = "";
  for (const j of list) {
    const row = document.createElement("div");
    row.className = "job-row";
    const url = document.createElement("div");
    url.className = "job-url";
    url.title = j.url || "";
    url.textContent = j.url || "(no url)";
    const meta = document.createElement("div");
    meta.className = "job-meta status-" + (j.status === "err" ? "err" : j.status === "done" ? "done" : "running");
    const tag = j.status === "done" ? "done" :
                j.status === "err"  ? ("err: " + (j.message || "")) :
                j.status === "abort" ? "aborted" :
                j.status === "running" ? jobSummary(j) :
                j.status || "queued";
    meta.textContent = tag;
    row.appendChild(url);
    row.appendChild(meta);
    if (j.status === "running") {
      const bar = document.createElement("div"); bar.className = "job-bar";
      const fill = document.createElement("div"); fill.className = "job-fill";
      fill.style.width = jobPercent(j).toFixed(1) + "%";
      bar.appendChild(fill);
      row.appendChild(bar);
    }
    container.appendChild(row);
  }
}

function fmtFormatLabel(f) {
  const parts = [];
  if (f.height) parts.push(f.height + "p" + (f.fps && f.fps !== 30 ? f.fps : ""));
  else if (f.quality) parts.push(f.quality);
  if (f.codecs) {
    const c = f.codecs.split(",")[0].trim();
    if (c) parts.push(c.split(".")[0]);
  } else if (f.container) {
    parts.push(f.container);
  }
  if (f.isAudio && !f.isVideo) {
    parts.unshift("audio");
    if (f.audioBitrate) parts.push(Math.round(f.audioBitrate/1000) + "k");
  }
  if (f.contentLength) {
    parts.push(fmtBytes(f.contentLength));
  } else if (f.bitrate) {
    parts.push(Math.round(f.bitrate/1000) + " kbps");
  }
  return parts.join(" · ");
}

function suggestFilename(meta, fmt) {
  const safe = (meta.title || "video").replace(/[\\/:*?"<>|]+/g, "_").slice(0, 120);
  const ext = fmt.container || (fmt.mimeType || "").split("/")[1] || "mp4";
  return `${safe}.${ext}`;
}

async function refreshYoutube() {
  const panel = $("ytPanel");
  const r = await sendBg({ action: "list-page-formats" });
  const data = (r && r.ok) ? r.data : null;
  if (!data || !data.formats || data.formats.length === 0) {
    panel.style.display = "none";
    return;
  }
  panel.style.display = "block";
  $("ytTitle").textContent = data.title || "(unknown)";
  $("ytTitle").title = data.title || "";
  const sel = $("ytQuality");
  sel.innerHTML = "";
  for (const f of data.formats) {
    const opt = document.createElement("option");
    opt.value = JSON.stringify({
      url: f.url,
      filename: suggestFilename(data, f),
      referer: data.pageUrl || ""
    });
    opt.textContent = fmtFormatLabel(f);
    sel.appendChild(opt);
  }
}

async function grabYoutube() {
  const sel = $("ytQuality");
  if (!sel.value) return;
  let chosen;
  try { chosen = JSON.parse(sel.value); } catch { return; }
  const outDir = $("outDir").value.trim();
  const status = $("ytStatus");
  status.textContent = "queued...";
  status.className = "muted";
  const r = await sendBg({
    action:   "grab-format",
    url:      chosen.url,
    filename: chosen.filename,
    referer:  chosen.referer,
    outDir
  });
  if (r && r.ok) {
    status.className = "ok";
    status.textContent = "queued: " + chosen.filename;
  } else {
    status.className = "err";
    status.textContent = "err: " + (r && r.err || "unknown");
  }
}

async function refreshSniffed() {
  const container = $("sniffList");
  const r = await sendBg({ action: "list-sniffed" });
  const list = (r && r.ok) ? (r.list || []) : [];
  if (list.length === 0) {
    container.textContent = "(none detected yet)";
    container.className = "muted";
    return;
  }
  container.className = "";
  container.innerHTML = "";
  for (const item of list) {
    const row = document.createElement("div");
    row.className = "sniff-row";
    const kind = document.createElement("span");
    kind.className = "sniff-kind";
    kind.textContent = item.type;
    const url = document.createElement("span");
    url.className = "sniff-url";
    url.title = item.url;
    url.textContent = item.url;
    const btn = document.createElement("button");
    btn.textContent = "grab";
    btn.addEventListener("click", async () => {
      btn.disabled = true; btn.textContent = "...";
      const outDir = $("outDir").value.trim();
      const resp = await sendBg({ action: "grab-sniffed",
                                  url: item.url,
                                  referer: item.referer,
                                  outDir });
      btn.textContent = (resp && resp.ok) ? "queued" : "err";
      setTimeout(() => { btn.disabled = false; btn.textContent = "grab"; }, 2000);
    });
    row.appendChild(kind);
    row.appendChild(url);
    row.appendChild(btn);
    container.appendChild(row);
  }
}

document.addEventListener("DOMContentLoaded", () => {
  load();
  refreshYoutube();
  refreshSniffed();
  refreshJobs();
  // Keep the active-downloads list fresh while the popup is open.
  setInterval(refreshJobs, 600);
  setInterval(refreshYoutube, 1500);
  $("save").addEventListener("click", save);
  $("send").addEventListener("click", sendManual);
  $("ping").addEventListener("click", ping);
  $("ytGrab").addEventListener("click", grabYoutube);
});
