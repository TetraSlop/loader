// The game files: the user picks their runtime folder (tmloader's, laid out as retail-files.txt says), the page keeps
// the files that list names, and the worker caches them in the origin private file system (OPFS) for the next visit.

const CACHE = "runtime";

/** The paths retail-files.txt lists, in its sha256sum format. */
export const retailPaths = text =>
  text.split("\n").map(l => l.trim()).filter(l => l && !l.startsWith("#")).map(l => l.slice(l.indexOf(" ")).trim());

/**
 * The listed files among entries ({ path, file }, paths from the picked folder), matched without case as a Windows
 * folder is, and named as the list spells them; and the listed paths that are not there.
 */
export function runtimeFiles(entries, paths) {
  const byKey = new Map(entries.map(e => [e.path.replaceAll("\\", "/").toLowerCase(), e.file]));
  const found = [], missing = [];
  for (const path of paths) {
    const file = byKey.get(path.toLowerCase());
    if (file) found.push({ path, file });
    else missing.push(path);
  }
  return { found, missing };
}

/** Every file under a FileSystemDirectoryHandle, as { path, file }. */
export async function walk(dir, prefix = "") {
  const out = [];
  for await (const [name, handle] of dir.entries()) {
    if (handle.kind === "directory") out.push(...await walk(handle, `${prefix}${name}/`));
    else out.push({ path: prefix + name, file: await handle.getFile() });
  }
  return out;
}

/** The files of an <input webkitdirectory>, as { path, file } from inside the picked folder. */
export const fromInput = files =>
  [...files].map(file => ({ path: file.webkitRelativePath.slice(file.webkitRelativePath.indexOf("/") + 1), file }));

const cacheDir = async create => {
  try {
    return await (await navigator.storage.getDirectory()).getDirectoryHandle(CACHE, { create });
  } catch {
    return undefined;
  }
};

/** In the worker: replaces the cache with these { path, bytes }. Sync access handles exist only in workers. */
export async function saveCache(files) {
  await clearCache();
  const root = await cacheDir(true);
  for (const { path, bytes } of files) {
    let dir = root;
    const parts = path.split("/");
    for (const part of parts.slice(0, -1)) dir = await dir.getDirectoryHandle(part, { create: true });
    const handle = await (await dir.getFileHandle(parts.at(-1), { create: true })).createSyncAccessHandle();
    handle.truncate(0);
    handle.write(bytes, { at: 0 });
    handle.close();
  }
}

/** The cached files as { path, file }; none when nothing is cached. */
export async function loadCache() {
  const dir = await cacheDir(false);
  return dir ? walk(dir) : [];
}

export async function clearCache() {
  await (await navigator.storage.getDirectory()).removeEntry(CACHE, { recursive: true }).catch(() => {});
}

export const hasCache = async () => (await cacheDir(false)) !== undefined;
