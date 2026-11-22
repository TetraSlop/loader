// The page: pick the runtime folder, start the module in a worker, show its log and its canvas.
import { clearCache, fromInput, hasCache, walk } from "./files.js";
import { forwardInput } from "./input.js";
import { createSpeakers } from "./speakers.js";

const $ = id => document.getElementById(id);
const [pick, folder, start, forget, status, screen, logView] = ["pick", "folder", "start", "forget", "status", "screen", "log"].map($);

/** The files picked this visit, as { path, file }; none to start from the cache. */
let picked;
const show = text => (status.textContent = text);
const append = line => {
  logView.textContent += `${line}\n`;
  logView.scrollTop = logView.scrollHeight;
};

const ready = async () => {
  const cached = await hasCache();
  start.disabled = !picked && !cached;
  forget.disabled = !cached;
  show(picked ? `${picked.length} files picked` : cached ? "Game files cached from an earlier visit" : "Pick your tmloader runtime folder");
};

// The File System Access API where there is one; a directory upload elsewhere
pick.onclick = async () => {
  if (!window.showDirectoryPicker) return folder.click();
  try {
    picked = await walk(await window.showDirectoryPicker({ id: "tmweb-runtime" }));
  } catch {
    return; // dismissed
  }
  await ready();
};
folder.onchange = async () => {
  picked = fromInput(folder.files);
  await ready();
};
forget.onclick = async () => {
  await clearCache();
  await ready();
};

start.onclick = () => {
  for (const b of [pick, start, forget]) b.disabled = true;
  const speakers = createSpeakers(); // in the click, which lets audio start
  const offscreen = screen.transferControlToOffscreen();
  const worker = new Worker(new URL("./worker.js", import.meta.url), { type: "module" });
  worker.onmessage = ({ data: m }) => {
    if (m.type === "log") append(m.line);
    else if (m.type === "sound") speakers.play(m.samples);
    else if (m.type === "state") {
      show({ running: "Running", halted: "Halted", failed: "Failed", "need-files": "Pick your runtime folder first" }[m.state]);
      if (m.state !== "running") speakers.close();
    }
  };
  worker.onerror = e => append(`worker: ${e.message}`);
  worker.postMessage({ type: "start", files: picked, canvas: offscreen }, [offscreen]);
  forwardInput(screen, m => worker.postMessage(m), () => 640 / 480);
  screen.focus();
};

await ready();
