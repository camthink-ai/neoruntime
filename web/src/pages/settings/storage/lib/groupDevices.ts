import type {
  DiskPartition,
  StorageDevice,
  StorageDeviceType,
} from '@/services/types';

function getParentDisk(device: string): string {
  const base = device.replace('/dev/', '');
  const mmcMatch = base.match(/^(mmcblk\d+)/);
  if (mmcMatch) return mmcMatch[1];
  const sdMatch = base.match(/^(sd[a-z])/);
  if (sdMatch) return sdMatch[1];
  return base;
}

function classifyDevice(partitions: DiskPartition[]): StorageDeviceType {
  if (partitions.some(p => p.is_system)) return 'internal';
  if (partitions.some(p => p.is_removable)) {
    const hasMmcblk = partitions.some(p => p.device.includes('mmcblk'));
    if (hasMmcblk) return 'sd_card';
    return 'usb';
  }
  return 'internal';
}

function getDeviceLabel(type: StorageDeviceType): string {
  switch (type) {
    case 'internal':
      return 'internal_storage';
    case 'sd_card':
      return 'sd_card';
    case 'usb':
      return 'usb_storage';
    default:
      return 'internal_storage';
  }
}

function buildDevice(id: string, partitions: DiskPartition[]): StorageDevice {
  const type = classifyDevice(partitions);
  const totalBytes = partitions.reduce((s, p) => s + p.total, 0);
  const usedBytes = partitions.reduce((s, p) => s + p.used, 0);
  const freeBytes = partitions.reduce((s, p) => s + p.free, 0);

  return {
    id,
    type,
    label: getDeviceLabel(type),
    totalBytes,
    usedBytes,
    freeBytes,
    usagePercent: totalBytes > 0 ? (usedBytes / totalBytes) * 100 : 0,
    partitions,
    canFormat: partitions.length > 0 && !partitions.some(p => p.is_protected),
    canUnmount: partitions.some(p => p.is_removable && p.mountpoint),
    hasUnmountedPartitions: partitions.some(p => !p.mountpoint),
  };
}

export function groupPartitionsByDevice(
  partitions: DiskPartition[]
): StorageDevice[] {
  // Deduplicate: same device may appear multiple times (e.g. mmcblk1p3 at /data and /mnt/mmcblk1p3)
  // Keep the entry with the primary mountpoint (protected > system > shortest path)
  const seen = new Map<string, DiskPartition>();
  for (const p of partitions) {
    const existing = seen.get(p.device);
    if (!existing) {
      seen.set(p.device, p);
    } else {
      // Prefer protected/system mountpoints over ad-hoc ones
      const existingPri =        (existing.is_protected ? 2 : 0) + (existing.is_system ? 1 : 0);
      const currentPri = (p.is_protected ? 2 : 0) + (p.is_system ? 1 : 0);
      if (currentPri > existingPri) seen.set(p.device, p);
    }
  }

  const deduped = Array.from(seen.values());
  const groups = new Map<string, DiskPartition[]>();

  for (const p of deduped) {
    const disk = getParentDisk(p.device);
    if (!groups.has(disk)) groups.set(disk, []);
    groups.get(disk)!.push(p);
  }

  const devices: StorageDevice[] = [];
  for (const [id, parts] of groups) {
    devices.push(buildDevice(id, parts));
  }

  // Always include SD card slot entry even if no card detected
  const hasSdCard = devices.some(d => d.type === 'sd_card');
  if (!hasSdCard) {
    devices.push({
      id: 'sd_card_slot',
      type: 'sd_card',
      label: 'sd_card',
      totalBytes: 0,
      usedBytes: 0,
      freeBytes: 0,
      usagePercent: 0,
      partitions: [],
      canFormat: false,
      canUnmount: false,
      hasUnmountedPartitions: false,
    });
  }

  const order: Record<StorageDeviceType, number> = {
    internal: 0,
    sd_card: 1,
    usb: 2,
  };
  devices.sort((a, b) => order[a.type] - order[b.type]);

  return devices;
}
