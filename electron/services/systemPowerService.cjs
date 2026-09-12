const fs = require('node:fs/promises');
const path = require('node:path');
const { exec } = require('node:child_process');
const { promisify } = require('node:util');

const execAsync = promisify(exec);

class SystemPowerService {
  async getBatteryStatus() {
    return {
      host: {
        available: false,
        percentage: null,
        state: 'unknown',
        onAc: false
      },
      devices: [],
      available: false,
      percentage: null,
      state: 'unknown',
      onAc: false
    };
  }

  startMonitoring(_callback, _intervalMs) {}
  stopMonitoring() {}
}

class LinuxSystemPowerService extends SystemPowerService {
  constructor(sysfsPath = '/sys/class/power_supply') {
    super();
    this.sysfsPath = sysfsPath;
    this.timer = null;
    this.lastStatusSignature = null;
  }

  async getHostBattery() {
    try {
      const entries = await fs.readdir(this.sysfsPath);
      let totalCapacity = 0;
      let batteryCount = 0;
      let state = 'unknown';
      let hasAc = false;

      for (const entry of entries) {
        const entryPath = path.join(this.sysfsPath, entry);
        try {
          const type = (await fs.readFile(path.join(entryPath, 'type'), 'utf8')).trim();

          const isBattery = type === 'Battery' || entry.toLowerCase().startsWith('bat');
          if (isBattery) {
            try {
              const capStr = (await fs.readFile(path.join(entryPath, 'capacity'), 'utf8')).trim();
              const cap = parseInt(capStr, 10);
              if (!Number.isNaN(cap)) {
                totalCapacity += cap;
                batteryCount++;
              }
            } catch {}

            try {
              const statusStr = (await fs.readFile(path.join(entryPath, 'status'), 'utf8')).trim();
              if (statusStr === 'Charging') {
                state = 'charging';
              } else if (statusStr === 'Discharging') {
                state = 'discharging';
              } else if (statusStr === 'Full') {
                state = 'full';
              }
            } catch {}
          } else if (type === 'Mains' || entry.startsWith('AC') || entry.startsWith('ADP')) {
            try {
              const onlineStr = (await fs.readFile(path.join(entryPath, 'online'), 'utf8')).trim();
              if (onlineStr === '1') {
                hasAc = true;
              }
            } catch {}
          }
        } catch {}
      }

      if (batteryCount > 0) {
        const percentage = Math.max(0, Math.min(100, Math.round(totalCapacity / batteryCount)));
        return {
          available: true,
          percentage,
          state,
          onAc: hasAc
        };
      }

      return {
        available: false,
        percentage: null,
        state: 'unknown',
        onAc: hasAc
      };
    } catch {
      return {
        available: false,
        percentage: null,
        state: 'unknown',
        onAc: false
      };
    }
  }

  async getBluetoothBatteries() {
    const devices = [];
    try {
      const { stdout: devOut } = await execAsync('bluetoothctl devices Connected', { timeout: 1500 });
      const lines = devOut.split('\n');
      for (const line of lines) {
        const match = line.match(/^Device\s+([0-9A-Fa-f:]+)\s+(.+)$/);
        if (match) {
          const mac = match[1].trim();
          const name = match[2].trim();
          try {
            const { stdout: infoOut } = await execAsync(`bluetoothctl info ${mac}`, { timeout: 1000 });
            const batteryMatch = infoOut.match(/Battery\s+Percentage:\s*(?:0x[0-9a-fA-F]+\s*\(([0-9]+)\)|([0-9]+)%?)/i);
            if (batteryMatch) {
              const percentage = parseInt(batteryMatch[1] || batteryMatch[2], 10);
              if (!Number.isNaN(percentage)) {
                devices.push({
                  id: mac,
                  name,
                  percentage: Math.max(0, Math.min(100, percentage)),
                  state: 'connected'
                });
                continue;
              }
            }
          } catch {}

          devices.push({
            id: mac,
            name,
            percentage: null,
            state: 'connected'
          });
        }
      }
    } catch {}

    return devices;
  }

  async getBatteryStatus() {
    const [hostBattery, btDevices] = await Promise.all([
      this.getHostBattery(),
      this.getBluetoothBatteries()
    ]);

    return {
      host: hostBattery,
      devices: btDevices,
      available: hostBattery.available,
      percentage: hostBattery.percentage,
      state: hostBattery.state,
      onAc: hostBattery.onAc
    };
  }

  startMonitoring(callback, intervalMs = 15000) {
    this.stopMonitoring();
    const poll = async () => {
      try {
        const current = await this.getBatteryStatus();
        const signature = JSON.stringify({
          host: current.host,
          devices: current.devices
        });

        if (this.lastStatusSignature !== signature) {
          this.lastStatusSignature = signature;
          if (typeof callback === 'function') {
            callback(current);
          }
        }
      } catch {}
    };

    poll();
    this.timer = setInterval(poll, intervalMs);
  }

  stopMonitoring() {
    if (this.timer) {
      clearInterval(this.timer);
      this.timer = null;
    }
  }
}

function createSystemPowerService(platform = process.platform, sysfsPath) {
  if (platform === 'linux') {
    return new LinuxSystemPowerService(sysfsPath);
  }

  return new SystemPowerService();
}

module.exports = {
  SystemPowerService,
  LinuxSystemPowerService,
  createSystemPowerService
};
