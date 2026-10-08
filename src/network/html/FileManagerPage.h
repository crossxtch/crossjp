#pragma once

// File manager page served by FileTransferServer. Plain HTML/CSS/JS, no build
// step and no external libraries — list/upload/download/mkdir/rename/move/delete.
static const char FILE_MANAGER_PAGE[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="dark">
<meta name="theme-color" content="#0a0a0a">
<title>crossjp file transfer</title>
<style>
:root {
  --bg: #0a0a0a;
  --panel: #141414;
  --lift: #1c1c1c;
  --line: rgba(255,255,255,.08);
  --line2: rgba(255,255,255,.16);
  --ink: #fcfcfc;
  --mute: #9e9e9e;
  --dim: #6c6c6c;
  --accent: #ff7a17;
  --accent-soft: rgba(255,122,23,.16);
  --good: #9ece6a;
  --bad: #f7768e;
  --font: ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif;
  --mono: ui-monospace, "SF Mono", Menlo, Consolas, monospace;
  --r: 16px;
}
* { box-sizing: border-box; }
html, body { margin: 0; min-height: 100%; }
body {
  font-family: var(--font);
  background: var(--bg);
  color: var(--ink);
  padding: 28px 20px 64px;
  -webkit-font-smoothing: antialiased;
}
body::before {
  content: "";
  position: fixed;
  inset: -30% 0 auto 0;
  height: 52vh;
  background: radial-gradient(ellipse at 50% 0%, rgba(255,122,23,.14), transparent 68%);
  pointer-events: none;
}
.wrap { position: relative; max-width: 720px; margin: 0 auto; }
.eyebrow {
  font-family: var(--mono);
  font-size: 11px;
  letter-spacing: 1.6px;
  text-transform: uppercase;
  color: var(--accent);
  margin-bottom: 8px;
}
h1 {
  font-size: 34px;
  font-weight: 500;
  letter-spacing: -0.04em;
  margin: 0 0 22px;
}
#path {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 4px;
  margin-bottom: 16px;
  font-family: var(--mono);
  font-size: 12px;
  color: var(--mute);
}
.crumb {
  background: none;
  border: 0;
  color: var(--mute);
  padding: 2px 6px;
  border-radius: 8px;
  font: inherit;
  cursor: pointer;
}
.crumb:hover { color: var(--ink); background: rgba(255,255,255,.06); }
.crumb.now { color: var(--ink); cursor: default; }
.sep { color: var(--dim); padding: 0 2px; }
#dropZone {
  border: 1px dashed var(--line2);
  border-radius: 22px;
  background: var(--panel);
  padding: 36px 20px;
  text-align: center;
  cursor: pointer;
  margin-bottom: 14px;
  transition: border-color .15s, background .15s, box-shadow .15s;
}
#dropZone:hover { border-color: var(--line2); background: var(--lift); }
#dropZone.dragover {
  border-color: var(--accent);
  background: var(--accent-soft);
  box-shadow: 0 0 0 4px var(--accent-soft);
}
#dropZone.busy { cursor: default; opacity: .72; }
.drop-kicker {
  font-family: var(--mono);
  font-size: 11px;
  letter-spacing: 1.4px;
  text-transform: uppercase;
  color: var(--accent);
  margin-bottom: 8px;
}
.drop-title { font-size: 18px; font-weight: 500; letter-spacing: -0.02em; }
.drop-sub { margin-top: 6px; color: var(--mute); font-size: 13px; }
#fileInput { display: none; }
#toolbar { display: flex; flex-wrap: wrap; gap: 8px; margin: 4px 0 16px; }
button, .btn {
  font-family: var(--mono);
  font-size: 11px;
  letter-spacing: .8px;
  text-transform: uppercase;
  color: var(--ink);
  background: transparent;
  border: 1px solid var(--line2);
  border-radius: 999px;
  padding: 8px 14px;
  cursor: pointer;
  text-decoration: none;
  display: inline-flex;
  align-items: center;
}
button:hover, .btn:hover { background: rgba(255,255,255,.06); border-color: rgba(255,255,255,.28); }
button.ghost { color: var(--mute); }
button.danger:hover { color: var(--bad); border-color: var(--bad); }
#cancelUpload { display: none; color: var(--accent); border-color: rgba(255,122,23,.4); }
#uploadProgress {
  display: none;
  height: 3px;
  background: var(--line);
  border-radius: 999px;
  overflow: hidden;
  margin: 0 0 14px;
}
#uploadBar {
  display: block;
  height: 100%;
  width: 0;
  background: linear-gradient(90deg, var(--accent), #ffc285);
  transition: width .12s linear;
}
.panel {
  background: var(--panel);
  border: 1px solid var(--line);
  border-radius: var(--r);
  overflow: hidden;
}
.panel-head {
  font-family: var(--mono);
  font-size: 11px;
  letter-spacing: 1.4px;
  text-transform: uppercase;
  color: var(--dim);
  padding: 12px 16px;
  border-bottom: 1px solid var(--line);
}
.row {
  display: grid;
  grid-template-columns: 28px 1fr auto;
  gap: 10px;
  align-items: center;
  padding: 12px 16px;
  border-bottom: 1px solid var(--line);
}
.row:last-child { border-bottom: 0; }
.row:hover { background: rgba(255,255,255,.03); }
.icon { width: 28px; height: 28px; color: var(--mute); display: flex; align-items: center; justify-content: center; }
.icon svg { width: 18px; height: 18px; }
.row.dir .icon { color: var(--accent); }
.meta { min-width: 0; }
.name { font-size: 14px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.row.dir .name { cursor: pointer; }
.row.dir .name:hover { color: var(--accent); }
.sub { font-family: var(--mono); font-size: 11px; color: var(--dim); margin-top: 2px; }
.actions { display: flex; flex-wrap: wrap; gap: 6px; justify-content: flex-end; }
.actions .btn, .actions button { padding: 6px 10px; font-size: 10px; }
.empty { padding: 36px 16px; text-align: center; color: var(--dim); font-size: 13px; }
.setting {
  display: grid;
  grid-template-columns: 1fr auto;
  gap: 12px;
  align-items: center;
  padding: 12px 16px;
}
select {
  font-family: var(--mono);
  font-size: 12px;
  color: var(--ink);
  background: var(--lift);
  border: 1px solid var(--line2);
  border-radius: 999px;
  padding: 8px 12px;
  max-width: 46vw;
}
#status { min-height: 20px; margin-top: 14px; font-size: 13px; color: var(--mute); }
#status.ok { color: var(--good); }
#status.bad { color: var(--bad); }
footer {
  margin-top: 28px;
  font-family: var(--mono);
  font-size: 11px;
  letter-spacing: .4px;
  color: var(--dim);
}
@media (max-width: 560px) {
  .row { grid-template-columns: 28px 1fr; }
  .actions { grid-column: 2; }
}
</style>
</head>
<body>
<div class="wrap">
  <div class="eyebrow">file transfer</div>
  <h1>crossjp</h1>
  <nav id="path"></nav>
  <div id="dropZone">
    <div class="drop-kicker">upload</div>
    <div class="drop-title">Drop a file</div>
    <div class="drop-sub">.epub · .txt · .xgf2 fonts · firmware .bin</div>
  </div>
  <input type="file" id="fileInput" accept=".epub,.txt,.xgf2,.bin">
  <div id="uploadProgress"><span id="uploadBar"></span></div>
  <div id="toolbar">
    <button id="cancelUpload" onclick="cancelUpload()">Cancel</button>
    <button class="ghost" onclick="mkdir()">New folder</button>
    <button class="ghost" onclick="load()">Refresh</button>
  </div>
  <section class="panel">
    <div class="panel-head">Files</div>
    <div id="list"></div>
  </section>
  <section class="panel" style="margin-top:16px">
    <div class="panel-head">Fonts</div>
    <div id="fonts"></div>
  </section>
  <section class="panel" style="margin-top:16px">
    <div class="panel-head">Device</div>
    <div class="setting">
      <div class="meta">
        <div class="name">Timezone</div>
        <div class="sub">UTC offset for the clock</div>
      </div>
      <select id="tzSelect"></select>
    </div>
  </section>
  <div id="status"></div>
  <footer id="foot">crossjp</footer>
</div>
<script>
let path = "/";
let activeUpload = null;
let retryTimer = null;
const ICO_DIR = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6"><path d="M3 7a2 2 0 012-2h5l2 2h7a2 2 0 012 2v8a2 2 0 01-2 2H5a2 2 0 01-2-2z"/></svg>';
const ICO_FILE = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6"><path d="M14 3H7a2 2 0 00-2 2v14a2 2 0 002 2h10a2 2 0 002-2V8z"/><path d="M14 3v5h5"/></svg>';

function status(msg, kind) {
  const el = document.getElementById("status");
  el.textContent = msg || "";
  el.className = kind || "";
}

function uploadBusy() { return activeUpload || retryTimer; }

function resetUploadUi() {
  activeUpload = null;
  if (retryTimer) { clearTimeout(retryTimer); retryTimer = null; }
  const dropZone = document.getElementById("dropZone");
  document.getElementById("uploadProgress").style.display = "none";
  document.getElementById("uploadBar").style.width = "0%";
  document.getElementById("cancelUpload").style.display = "none";
  dropZone.classList.remove("busy");
  dropZone.querySelector(".drop-title").textContent = "Drop a file";
  dropZone.querySelector(".drop-sub").textContent = ".epub · .txt · .xgf2 fonts · firmware .bin";
  document.getElementById("fileInput").value = "";
}

function formatSize(bytes) {
  if (bytes < 1024) return bytes + " B";
  const units = ["KB", "MB", "GB"];
  let value = bytes / 1024;
  let i = 0;
  while (value >= 1024 && i < units.length - 1) { value /= 1024; i++; }
  return value.toFixed(value < 10 ? 1 : 0) + " " + units[i];
}

function renderCrumbs() {
  const nav = document.getElementById("path");
  nav.innerHTML = "";
  const addBtn = (label, target) => {
    const b = document.createElement("button");
    b.className = "crumb";
    b.textContent = label;
    b.onclick = () => { path = target; load(); };
    nav.appendChild(b);
  };
  const addSep = () => {
    const s = document.createElement("span");
    s.className = "sep";
    s.textContent = "/";
    nav.appendChild(s);
  };
  if (path === "/") {
    const now = document.createElement("span");
    now.className = "crumb now";
    now.textContent = "/";
    nav.appendChild(now);
    return;
  }
  addBtn("/", "/");
  const parts = path.split("/").filter(Boolean);
  let acc = "";
  parts.forEach((part, i) => {
    addSep();
    acc += "/" + part;
    if (i === parts.length - 1) {
      const now = document.createElement("span");
      now.className = "crumb now";
      now.textContent = part;
      nav.appendChild(now);
    } else {
      addBtn(part, acc);
    }
  });
}

function actionBtn(label, onClick, extra) {
  const b = document.createElement("button");
  b.textContent = label;
  if (extra) b.className = extra;
  b.onclick = onClick;
  return b;
}

function load() {
  renderCrumbs();
  fetch("/api/files?path=" + encodeURIComponent(path))
    .then(r => r.json())
    .then(items => {
      const list = document.getElementById("list");
      list.innerHTML = "";
      if (!items.length) {
        const empty = document.createElement("div");
        empty.className = "empty";
        empty.textContent = "This folder is empty";
        list.appendChild(empty);
        return;
      }
      items.forEach(item => {
        const row = document.createElement("div");
        row.className = "row" + (item.isDirectory ? " dir" : "");
        const icon = document.createElement("div");
        icon.className = "icon";
        icon.innerHTML = item.isDirectory ? ICO_DIR : ICO_FILE;
        const meta = document.createElement("div");
        meta.className = "meta";
        const name = document.createElement("div");
        name.className = "name";
        name.textContent = item.name;
        const sub = document.createElement("div");
        sub.className = "sub";
        sub.textContent = item.isDirectory ? "folder" : formatSize(item.size);
        meta.appendChild(name);
        meta.appendChild(sub);
        const actions = document.createElement("div");
        actions.className = "actions";
        const full = (path === "/" ? "" : path) + "/" + item.name;
        if (item.isDirectory) {
          name.onclick = () => { path = full; load(); };
        } else {
          const dl = document.createElement("a");
          dl.className = "btn";
          dl.href = "/download?path=" + encodeURIComponent(full);
          dl.textContent = "Get";
          actions.appendChild(dl);
        }
        actions.appendChild(actionBtn("Rename", () => rename(full, item.name)));
        actions.appendChild(actionBtn("Move", () => move_(full)));
        actions.appendChild(actionBtn("Delete", () => del_(full), "danger"));
        row.appendChild(icon);
        row.appendChild(meta);
        row.appendChild(actions);
        list.appendChild(row);
      });
    })
    .catch(e => status("Error: " + e, "bad"));
  loadFonts();
}

function loadFonts() {
  fetch("/api/fonts")
    .then(r => r.json())
    .then(items => {
      const list = document.getElementById("fonts");
      list.innerHTML = "";
      if (!items.length) {
        const empty = document.createElement("div");
        empty.className = "empty";
        empty.textContent = "No fonts yet — drop a .xgf2 file";
        list.appendChild(empty);
        return;
      }
      items.sort((a, b) => a.name.localeCompare(b.name));
      items.forEach(item => {
        const row = document.createElement("div");
        row.className = "row";
        const meta = document.createElement("div");
        meta.className = "meta";
        const name = document.createElement("div");
        name.className = "name";
        name.textContent = item.name + (item.active ? "  ·  in use" : "");
        const sub = document.createElement("div");
        sub.className = "sub";
        sub.textContent = formatSize(item.size);
        meta.appendChild(name);
        meta.appendChild(sub);
        const actions = document.createElement("div");
        actions.className = "actions";
        if (!item.active) {
          actions.appendChild(actionBtn("Use", () => {
            fetch("/api/fonts/select", {
              method: "POST",
              headers: { "Content-Type": "application/x-www-form-urlencoded" },
              body: "name=" + encodeURIComponent(item.name)
            }).then(r => { if (!r.ok) throw new Error("select failed"); loadFonts(); status("Using " + item.name, "ok"); })
              .catch(e => status("Error: " + e, "bad"));
          }));
        }
        actions.appendChild(actionBtn("Delete", () => {
          if (!confirm("Delete " + item.name + "?")) return;
          fetch("/api/fonts/delete", {
            method: "POST",
            headers: { "Content-Type": "application/x-www-form-urlencoded" },
            body: "name=" + encodeURIComponent(item.name)
          }).then(r => { if (!r.ok) throw new Error("delete failed"); loadFonts(); status("Deleted " + item.name, "ok"); })
            .catch(e => status("Error: " + e, "bad"));
        }, "danger"));
        row.appendChild(meta);
        row.appendChild(actions);
        list.appendChild(row);
      });
    })
    .catch(e => status("Error: " + e, "bad"));
}

function upload(file, attempt) {
  if (!file) return;
  if (file.name.length > 255) {
    status("Filename too long (max 255 characters)", "bad");
    return;
  }
  if (uploadBusy()) { status("Upload already in progress", "bad"); return; }

  const wrap = document.getElementById("uploadProgress");
  const bar = document.getElementById("uploadBar");
  const cancelBtn = document.getElementById("cancelUpload");
  const dropZone = document.getElementById("dropZone");
  wrap.style.display = "block";
  bar.style.width = "0%";
  cancelBtn.style.display = "inline-flex";
  dropZone.classList.add("busy");
  dropZone.querySelector(".drop-title").textContent = file.name;
  dropZone.querySelector(".drop-sub").textContent = attempt ? "retrying" : "sending to device";

  let lastTime = performance.now();
  let lastLoaded = 0;
  let speed = 0;

  const xhr = new XMLHttpRequest();
  activeUpload = xhr;
  xhr.timeout = 10 * 60 * 1000;
  xhr.upload.onprogress = (e) => {
    const now = performance.now();
    const dt = (now - lastTime) / 1000;
    if (dt > 0.2) {
      const instantSpeed = (e.loaded - lastLoaded) / dt;
      speed = speed ? speed * 0.7 + instantSpeed * 0.3 : instantSpeed;
      lastTime = now;
      lastLoaded = e.loaded;
    }
    if (e.lengthComputable) {
      const pct = Math.round((e.loaded / e.total) * 100);
      bar.style.width = pct + "%";
      status("Uploading " + pct + "%  ·  " + formatSize(e.loaded) + " / " + formatSize(e.total) +
             "  ·  " + formatSize(speed) + "/s");
    } else {
      status("Uploading  ·  " + formatSize(e.loaded) + "  ·  " + formatSize(speed) + "/s");
    }
  };
  xhr.onload = () => {
    const ok = xhr.status >= 200 && xhr.status < 300;
    resetUploadUi();
    status(xhr.responseText, ok ? "ok" : "bad");
    load();
  };
  xhr.onerror = () => {
    activeUpload = null;
    if (!attempt) {
      status("Connection dropped, retrying…");
      retryTimer = setTimeout(() => { retryTimer = null; upload(file, 1); }, 2000);
      return;
    }
    resetUploadUi();
    status("Upload failed — wait a moment and try again", "bad");
  };
  xhr.ontimeout = () => {
    resetUploadUi();
    status("Upload timed out — check the device and retry", "bad");
  };
  xhr.onabort = () => { resetUploadUi(); status("Upload canceled"); load(); };
  xhr.open("POST", "/upload");
  xhr.setRequestHeader("Content-Type", "application/octet-stream");
  xhr.setRequestHeader("X-File-Path", encodeURIComponent(path));
  xhr.setRequestHeader("X-File-Name", encodeURIComponent(file.name));
  xhr.send(file);
}

function cancelUpload() {
  if (activeUpload) activeUpload.abort();
  else {
    resetUploadUi();
    status("Upload canceled");
    load();
  }
}

function mkdir() {
  const name = prompt("Folder name:");
  if (!name) return;
  fetch("/mkdir", { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: "name=" + encodeURIComponent(name) + "&path=" + encodeURIComponent(path) })
    .then(() => load())
    .catch(e => status("Error: " + e, "bad"));
}

function rename(fullPath, oldName) {
  const name = prompt("New name:", oldName);
  if (!name || name === oldName) return;
  fetch("/rename", { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: "path=" + encodeURIComponent(fullPath) + "&name=" + encodeURIComponent(name) })
    .then(() => load())
    .catch(e => status("Error: " + e, "bad"));
}

function move_(fullPath) {
  const dest = prompt("Destination folder (e.g. /Books):", path);
  if (!dest) return;
  fetch("/move", { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: "path=" + encodeURIComponent(fullPath) + "&dest=" + encodeURIComponent(dest) })
    .then(() => load())
    .catch(e => status("Error: " + e, "bad"));
}

function del_(fullPath) {
  if (!confirm("Delete " + fullPath + "?")) return;
  fetch("/delete", { method: "POST", headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: "path=" + encodeURIComponent(fullPath) })
    .then(r => r.text().then(t => {
      if (!r.ok) throw new Error(t || "delete failed");
      status("Deleted " + fullPath, "ok");
      load();
    }))
    .catch(e => status("Error: " + e.message, "bad"));
}

const dropZone = document.getElementById("dropZone");
const fileInput = document.getElementById("fileInput");

dropZone.addEventListener("click", () => {
  if (!uploadBusy()) fileInput.click();
});
fileInput.addEventListener("change", () => {
  if (fileInput.files.length) upload(fileInput.files[0]);
});
["dragenter", "dragover"].forEach(ev => {
  dropZone.addEventListener(ev, e => {
    e.preventDefault();
    if (!uploadBusy()) dropZone.classList.add("dragover");
  });
});
dropZone.addEventListener("dragleave", () => dropZone.classList.remove("dragover"));
dropZone.addEventListener("drop", e => {
  e.preventDefault();
  dropZone.classList.remove("dragover");
  const files = e.dataTransfer && e.dataTransfer.files;
  if (files && files.length) upload(files[0]);
});
document.addEventListener("dragover", e => e.preventDefault());
document.addEventListener("drop", e => e.preventDefault());

function tzLabel(q) {
  const mins = (q - 48) * 15;
  const sign = mins < 0 ? "-" : "+";
  const abs = Math.abs(mins);
  return "UTC" + sign + Math.floor(abs / 60) + ":" + String(abs % 60).padStart(2, "0");
}

const tzSelect = document.getElementById("tzSelect");
for (let q = 0; q <= 104; q++) {
  const opt = document.createElement("option");
  opt.value = q;
  opt.textContent = tzLabel(q);
  tzSelect.appendChild(opt);
}
tzSelect.onchange = () => {
  fetch("/api/timezone", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: "offsetQ=" + encodeURIComponent(tzSelect.value)
  })
    .then(r => {
      if (!r.ok) throw new Error("save failed");
      status("Timezone set to " + tzLabel(+tzSelect.value), "ok");
    })
    .catch(e => status("Error: " + e, "bad"));
};

fetch("/api/status").then(r => r.json()).then(s => {
  const bits = ["crossjp"];
  if (s.version) bits.push(s.version);
  if (s.ssid) bits.push(s.ssid);
  if (s.ip) bits.push(s.ip);
  document.getElementById("foot").textContent = bits.join("  ·  ");
  if (s.utcOffsetQ !== undefined) tzSelect.value = String(s.utcOffsetQ);
}).catch(() => {});

load();
</script>
</body>
</html>
)HTML";
