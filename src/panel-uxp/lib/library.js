// Reference library and saved looks, kept as JSON in the shared data folder
// (~/Library/Application Support/PRO PLUGS/LookMatch or %APPDATA%\PRO PLUGS\LookMatch).
// `fs` is any object with promise-returning readFile/writeFile/mkdir (UXP fs or node fs.promises).

const IMAGE_EXT = /\.(png|jpe?g|bmp|tga)$/i;
const LOOK_VERSION = 1;

function joinPath(dir, name) {
  return dir.replace(/[\\/]+$/, "") + (dir.includes("\\") ? "\\" : "/") + name;
}

function baseName(path) {
  return String(path).split(/[\\/]/).pop();
}

class Library {
  constructor(fs, dataDir) {
    this.fs = fs;
    this.dir = dataDir;
    this.refs = [];   // [{ id, path, name, added }]
    this.looks = [];  // [{ id, name, params, reference, created }]
  }

  get file() { return joinPath(this.dir, "library.json"); }

  async load() {
    try {
      const data = JSON.parse(await this.fs.readFile(this.file, { encoding: "utf-8" }));
      this.refs = Array.isArray(data.refs) ? data.refs.filter((r) => r && r.path) : [];
      this.looks = Array.isArray(data.looks) ? data.looks.filter((l) => l && l.name && l.params) : [];
    } catch (e) {
      this.refs = [];
      this.looks = [];
    }
    return this;
  }

  async save() {
    try { await this.fs.mkdir(this.dir, { recursive: true }); } catch (e) { /* exists */ }
    await this.fs.writeFile(this.file, JSON.stringify({ refs: this.refs, looks: this.looks }, null, 2),
                            { encoding: "utf-8" });
  }

  addReference(path) {
    if (!IMAGE_EXT.test(path)) throw new Error(`not a PNG, JPEG, BMP or TGA image: ${baseName(path)}`);
    const existing = this.refs.find((r) => r.path === path);
    if (existing) return existing;
    const ref = { id: `r${Date.now().toString(36)}${Math.floor(Math.random() * 1e4)}`, path, name: baseName(path),
                  added: new Date().toISOString() };
    this.refs.unshift(ref);
    return ref;
  }

  removeReference(id) {
    this.refs = this.refs.filter((r) => r.id !== id);
  }

  saveLook(name, params, reference) {
    const clean = String(name || "").trim() || `Look ${this.looks.length + 1}`;
    const look = { id: `l${Date.now().toString(36)}${Math.floor(Math.random() * 1e4)}`, name: clean,
                   params: Object.assign({}, params), reference: reference || null, created: new Date().toISOString() };
    this.looks = this.looks.filter((l) => l.name !== clean);
    this.looks.unshift(look);
    return look;
  }

  removeLook(id) {
    this.looks = this.looks.filter((l) => l.id !== id);
  }

  // .lmlook files: a single look as JSON, for sharing between machines.
  static lookToFile(look) {
    return JSON.stringify({ lookmatch: LOOK_VERSION, name: look.name, params: look.params,
                            reference: look.reference || null }, null, 2);
  }

  static lookFromFile(text) {
    const data = JSON.parse(text);
    if (!data || data.lookmatch !== LOOK_VERSION || !data.params || typeof data.params !== "object")
      throw new Error("not a LookMatch .lmlook file");
    for (const [k, v] of Object.entries(data.params))
      if (typeof v !== "number" && typeof v !== "boolean") throw new Error(`bad value for ${k}`);
    return { name: String(data.name || "Imported look"), params: data.params, reference: data.reference || null };
  }
}

module.exports = { Library, joinPath, baseName, IMAGE_EXT };
