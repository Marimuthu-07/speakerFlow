const fs = require('node:fs');
const path = require('node:path');

function resolveConfigDirectory(customDir) {
  if (customDir) return customDir;
  if (process.env.SPEAKERFLOW_CONFIG_DIR) return process.env.SPEAKERFLOW_CONFIG_DIR;

  try {
    const electron = require('electron');
    if (electron && electron.app && typeof electron.app.getPath === 'function') {
      return electron.app.getPath('userData');
    }
  } catch {}

  const fallbackBase = process.env.APPDATA || process.env.HOME || process.cwd();
  return path.join(fallbackBase, '.speakerflow');
}

class DeviceDelayStore {
  constructor(options = {}) {
    const configDir = resolveConfigDirectory(options.configDir);
    this.storagePath = options.storagePath || path.join(configDir, 'speakerflow-device-delays.json');
    this.delays = new Map();
    this.load();
  }

  load() {
    this.delays.clear();
    try {
      if (fs.existsSync(this.storagePath)) {
        const raw = fs.readFileSync(this.storagePath, 'utf8');
        const parsed = JSON.parse(raw);
        if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) {
          for (const [deviceId, val] of Object.entries(parsed)) {
            const num = Number(val);
            if (typeof deviceId === 'string' && deviceId.trim().length > 0 &&
                Number.isFinite(num) && num >= 0 && num <= 500) {
              this.delays.set(deviceId.trim(), Math.round(num * 10) / 10);
            }
          }
        }
      }
    } catch (err) {
      console.warn(`[DeviceDelayStore] Warning: Could not read persisted delays from ${this.storagePath}: ${err.message}`);
    }
  }

  getDelay(deviceId) {
    if (!deviceId || typeof deviceId !== 'string') return 0;
    const key = deviceId.trim();
    if (this.delays.has(key)) {
      return this.delays.get(key);
    }
    return 0;
  }

  setDelay(deviceId, delayMs) {
    if (!deviceId || typeof deviceId !== 'string' || deviceId.trim().length === 0) {
      throw new Error('A valid device endpoint identifier is required.');
    }
    const val = Number(delayMs);
    if (Number.isNaN(val) || !Number.isFinite(val) || val < 0 || val > 500) {
      throw new Error('Speaker delay must be a finite number between 0 and 500 milliseconds.');
    }

    const key = deviceId.trim();
    const normalizedVal = Math.round(val * 10) / 10;
    this.delays.set(key, normalizedVal);
    this.save();
    return normalizedVal;
  }

  save() {
    try {
      const dir = path.dirname(this.storagePath);
      if (!fs.existsSync(dir)) {
        fs.mkdirSync(dir, { recursive: true });
      }

      const data = Object.fromEntries(this.delays);
      const content = JSON.stringify(data, null, 2);
      const tmpPath = `${this.storagePath}.tmp.${Date.now()}.${Math.random().toString(36).slice(2, 8)}`;
      fs.writeFileSync(tmpPath, content, 'utf8');
      fs.renameSync(tmpPath, this.storagePath);
    } catch (err) {
      console.error(`[DeviceDelayStore] Failed to write persisted delays to ${this.storagePath}: ${err.message}`);
    }
  }

  getAll() {
    return Object.fromEntries(this.delays);
  }

  clear() {
    this.delays.clear();
    try {
      if (fs.existsSync(this.storagePath)) {
        fs.unlinkSync(this.storagePath);
      }
    } catch {}
  }
}

module.exports = {
  DeviceDelayStore,
  resolveConfigDirectory
};
