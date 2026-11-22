const el = (id) => document.getElementById(id);
const selected = new Set();
let busy = false;
let refreshVersion = 0;
let currentPolicy = "fcfs";

async function request(path, body) {
  const response = await fetch(path, body === undefined ? {} : {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || "The request failed.");
  return data;
}

function updateSubmit() {
  el("submit").disabled = busy || selected.size === 0;
  el("submit").textContent = selected.size ? `Queue ${selected.size} file(s)` : "Queue files";
}

async function loadFiles() {
  const { files } = await request("/api/files");
  const names = new Set(files.map(({ name }) => name));
  for (const name of selected) if (!names.has(name)) selected.delete(name);
  const items = files.map(({ name, size }) => {
    const row = document.createElement("label");
    const box = document.createElement("input");
    box.type = "checkbox";
    box.value = name;
    box.checked = selected.has(name);
    box.addEventListener("change", () => {
      if (box.checked) selected.add(name);
      else selected.delete(name);
      updateSubmit();
    });
    const label = document.createElement("span");
    label.textContent = name;
    const detail = document.createElement("small");
    detail.textContent = `${size.toLocaleString()} B`;
    row.append(box, label, detail);
    return row;
  });
  el("files").replaceChildren(...items);
  if (!items.length) el("files").textContent = "Copy files into the input folder, then refresh.";
  updateSubmit();
}

function time(ms) {
  if (ms === null) return "—";
  return ms < 1000 ? `${ms.toFixed(1)} ms` : `${(ms / 1000).toFixed(2)} s`;
}

function renderTimeline(jobs) {
  el("timeline").hidden = !jobs.length;
  el("timeline-empty").hidden = Boolean(jobs.length);
  if (!jobs.length) {
    el("timeline-rows").replaceChildren();
    return;
  }

  const span = Math.max(1, ...jobs.map((job) => job.elapsed_ms));
  const percent = (ms) => `${(ms / span) * 100}%`;
  const tick = (fraction) => {
    const label = document.createElement("span");
    label.textContent = time(span * fraction);
    return label;
  };
  el("timeline-axis").replaceChildren(...[0, 0.25, 0.5, 0.75, 1].map(tick));

  function segment(kind, start, length) {
    const bar = document.createElement("span");
    bar.className = `timeline-bar ${kind}`;
    bar.style.left = percent(start);
    bar.style.width = percent(length);
    return bar;
  }

  const rows = document.createDocumentFragment();
  for (const job of jobs) {
    const name = document.createElement("span");
    name.className = "timeline-name";
    name.textContent = `#${job.id} ${job.file}`;
    name.title = job.file;
    const state = document.createElement("small");
    state.className = `state ${job.state.toLowerCase()}`;
    state.textContent = job.state;
    name.append(state);
    const track = document.createElement("div");
    track.className = "timeline-track";
    const details = `Job ${job.id}, ${job.file}. ${job.state}. Submitted at ${time(job.submitted_ms)}. Queued for ${time(job.wait_ms)}.`
      + (job.run_ms === null ? "" : ` Processing time ${time(job.run_ms)}.`);
    track.setAttribute("role", "img");
    track.setAttribute("aria-label", details);
    track.title = details;
    const end = job.timeline_truncated ? job.slices.at(-1)[1] : job.elapsed_ms;
    track.append(segment("waiting", job.submitted_ms, end - job.submitted_ms));
    for (const [start, finish] of job.slices)
      track.append(segment("processing", start, finish - start));
    rows.append(name, track);
  }
  el("timeline-rows").replaceChildren(rows);
}

function row(job) {
  const tr = document.createElement("tr");
  function cell(value, className) {
    const td = tr.insertCell();
    td.textContent = value;
    if (className) td.className = className;
    return td;
  }
  cell(job.file, "filename");
  const status = cell(job.state, `state ${job.state.toLowerCase()}`);
  if (job.error) {
    const detail = document.createElement("small");
    detail.textContent = job.error;
    status.append(detail);
  }
  const checksum = cell(job.checksum ?? "—", "numeric");
  if (job.bytes !== null) {
    const detail = document.createElement("small");
    detail.textContent = `${job.bytes.toLocaleString()} B`;
    checksum.append(detail);
  }
  cell(job.pid ?? "—", "numeric");
  const memory = cell(job.peak_rss_kib == null ? "-" : `${job.peak_rss_kib} KiB`, "numeric");
  if (job.memory_limit_bytes != null)
    memory.title = `Address-space limit: ${job.memory_limit_bytes / (1024 * 1024)} MiB`;
  cell(time(job.submitted_ms), "numeric");
  cell(time(job.run_ms), "numeric");
  cell(time(job.completion_ms), "numeric");
  cell(time(job.turnaround_ms), "numeric");
  cell(time(job.wait_ms), "numeric");
  const action = cell("");
  if (job.state === "Queued") {
    const cancel = document.createElement("button");
    cancel.type = "button";
    cancel.textContent = "Cancel";
    cancel.setAttribute("aria-label", `Cancel job ${job.id} for ${job.file}`);
    cancel.addEventListener("click", () => perform(() => request("/api/cancel", { id: job.id })));
    action.append(cancel);
  } else if (job.exit_code !== null) {
    action.textContent = `Exit ${job.exit_code}`;
  }
  return tr;
}

async function loadJobs() {
  const version = ++refreshVersion;
  try {
    const snapshot = await request("/api/jobs");
    if (version !== refreshVersion) return;
    el("connection").textContent = "Live";
    el("slots").textContent = `${snapshot.running}/${snapshot.slots} slots active · Peak ${snapshot.peak} · ${snapshot.workers} workers`;
    el("deadlock-type").textContent = snapshot.deadlocks.type;
    el("deadlock-recovery").textContent = snapshot.deadlocks.recovery;
    el("deadlocks").textContent = snapshot.deadlocks.detected;
    const lastCycle = el("last-cycle");
    if (lastCycle.textContent !== snapshot.deadlocks.last_cycle) {
      lastCycle.textContent = snapshot.deadlocks.last_cycle;
      lastCycle.hidden = !snapshot.deadlocks.last_cycle;
    }
    currentPolicy = snapshot.policy;
    if (!busy && document.activeElement !== el("policy")) el("policy").value = currentPolicy;
    el("empty").hidden = snapshot.jobs.length > 0;
    // Keep the active cancel control in place while the user is navigating by keyboard.
    const focus = document.activeElement;
    const label = el("jobs").contains(focus) ? focus.getAttribute("aria-label") : null;
    const jobs = snapshot.jobs.slice().sort((a, b) =>
      (a.completion_ms ?? Infinity) - (b.completion_ms ?? Infinity) || a.id - b.id);
    el("jobs").replaceChildren(...jobs.map(row));
    renderTimeline(jobs);
    if (label) for (const button of el("jobs").querySelectorAll("button")) {
      if (button.getAttribute("aria-label") === label) button.focus();
    }
  } catch (error) {
    if (version === refreshVersion) el("connection").textContent = "Disconnected";
  }
}

async function perform(action) {
  if (busy) return;
  busy = true;
  updateSubmit();
  el("message").textContent = "";
  try {
    await action();
  } catch (error) {
    el("message").textContent = error.message;
  } finally {
    busy = false;
    updateSubmit();
    await loadJobs();
  }
}

el("refresh").addEventListener("click", () => perform(loadFiles));
el("submit").addEventListener("click", () => perform(async () => {
  if (!el("priority").reportValidity()) return;
  const names = Array.from(selected);
  const { ids } = await request("/api/jobs", {
    files: names,
    priority: Number(el("priority").value),
  });
  for (const name of names) selected.delete(name);
  for (const box of el("files").querySelectorAll("input")) box.checked = selected.has(box.value);
  el("message").textContent = `Queued ${ids.length} job(s).`;
}));
el("policy").addEventListener("change", () => {
  const value = el("policy").value;
  if (busy) {
    el("policy").value = currentPolicy;
    return;
  }
  perform(async () => {
    try {
      await request("/api/policy", { policy: value });
    } catch (error) {
      el("policy").value = currentPolicy;
      throw error;
    }
  });
});
el("clear").addEventListener("click", () => perform(() => request("/api/clear", {})));

loadFiles().catch((error) => { el("message").textContent = error.message; });
(async function poll() {
  await loadJobs();
  setTimeout(poll, 500);
})();
