import { useState, useEffect, useCallback, useMemo } from 'react'
import { EventsOn, EventsOff } from '../../wailsjs/runtime/runtime'
import {
  GetDevices,
  StartDiscovery,
  StopDiscovery,
  ScanDevices,
  Ping,
  GetListenerStats,
  ProbeManualDevice,
} from '../../wailsjs/go/main/App'

export interface Device {
  sn: string
  product: string
  ip: string
  port: number
  fw: string
  caps: string[]
  hw: string
  mac: string
  online: boolean
  lastSeen: string
  firstSeen: string
  manual?: boolean
}

export interface ListenerStats {
  recvCount: number
  decodeErrs: number
  eventCount: number
  running: boolean
  ifaces: string[]
}

export interface ManualProbeOptions {
  apiScheme: 'http' | 'https'
  apiPort: number
  username: string
  token: string
  skipTLSVerify: boolean
}

export function useDevices() {
  const [discoveredDevices, setDiscoveredDevices] = useState<Device[]>([])
  const [manualDevices, setManualDevices] = useState<Device[]>(loadManualDevices)
  const [selectedMacs, setSelectedMacs] = useState<Set<string>>(new Set())
  const [selectedMac, setSelectedMac] = useState<string | null>(null)
  const [scanning, setScanning] = useState(false)
  const [listening, setListening] = useState(false)
  const [status, setStatus] = useState("Initializing...")
  const [listenerStats, setListenerStats] = useState<ListenerStats | null>(null)

  const devices = useMemo(() => mergeDevices(manualDevices, discoveredDevices), [manualDevices, discoveredDevices])

  const refresh = useCallback(async () => {
    try {
      const list = await GetDevices()
      setDiscoveredDevices(list || [])
    } catch (e: unknown) {
      setStatus("Failed to get devices: " + String(e))
    }
  }, [])

  useEffect(() => {
    saveManualDevices(manualDevices)
  }, [manualDevices])

  const start = useCallback(async (iface: string) => {
    try {
      const pong = await Ping()
      console.log("[hook] Ping:", pong)

      const result = await StartDiscovery(iface)
      console.log("[hook] StartDiscovery:", result)
      setStatus("Listener: " + result)
      setListening(true)
      await refresh()
    } catch (e: unknown) {
      const msg = String(e)
      setStatus("Start failed: " + msg)
      console.error("[hook] StartDiscovery error:", msg)
    }
  }, [refresh])

  const stop = useCallback(async () => {
    try {
      await StopDiscovery()
      setListening(false)
      setStatus("Listener stopped")
    } catch (e: unknown) {
      console.error("[hook] StopDiscovery error:", e)
    }
  }, [])

  const scan = useCallback(async (iface: string) => {
    setScanning(true)
    setStatus("Scanning...")
    try {
      const result = await ScanDevices(iface)
      console.log("[hook] ScanDevices:", result)
      setStatus("Scan: " + result)
      await new Promise(r => setTimeout(r, 3000))
      await refresh()
      try {
        const stats = await GetListenerStats()
        console.log("[hook] ListenerStats:", stats)
        setListenerStats(stats)
        setStatus(`Scan complete | Rx:${stats.recvCount} Errs:${stats.decodeErrs} Events:${stats.eventCount} Listening:${stats.running}`)
      } catch {
        // ignore stats error
      }
    } catch (e: unknown) {
      const msg = String(e)
      setStatus("Scan failed: " + msg)
      console.error("[hook] ScanDevices error:", msg)
    } finally {
      setScanning(false)
    }
  }, [refresh])

  useEffect(() => {
    EventsOn("device:online", () => {
      console.log("[hook] event: device:online")
      refresh()
    })
    EventsOn("device:update", () => refresh())
    EventsOn("device:offline", () => refresh())
    return () => {
      EventsOff("device:online")
      EventsOff("device:update")
      EventsOff("device:offline")
    }
  }, [refresh])

  // Single-device selection (for detail panel)
  const selectedDevice = devices.find(d => deviceKey(d) === selectedMac) || null

  // Batch selection helpers
  const toggleSelect = (key: string) => {
    setSelectedMacs(prev => {
      const next = new Set(prev)
      if (next.has(key)) {
        next.delete(key)
      } else {
        next.add(key)
      }
      return next
    })
  }

  const selectAll = () => {
    const onlineKeys = devices.filter(d => d.online && deviceKey(d)).map(deviceKey)
    if (onlineKeys.length === 0) return
    // Toggle: if all online are already selected, clear instead
    const allSelected = onlineKeys.every(key => selectedMacs.has(key))
    setSelectedMacs(allSelected ? new Set() : new Set(onlineKeys))
  }

  const clearSelection = () => {
    setSelectedMacs(new Set())
  }

  const isSelected = (key: string) => selectedMacs.has(key)

  const batchDevices = devices.filter(d => selectedMacs.has(deviceKey(d)))

  const addManualDevices = useCallback(async (value: string, options: ManualProbeOptions) => {
    const parsed = parseManualDevices(value, options)
    if (parsed.length === 0) {
      setStatus("No valid manual devices to add")
      return 0
    }
    setManualDevices(prev => upsertDevices(prev, parsed))
    setStatus(`Added ${parsed.length} manual device${parsed.length === 1 ? '' : 's'}; probing device info...`)

    const results = await Promise.all(parsed.map(async device => {
      try {
        const probed = await ProbeManualDevice(JSON.stringify({
          host: device.ip,
          apiScheme: options.apiScheme,
          apiPort: device.port || options.apiPort,
          username: options.username,
          token: options.token,
          skipTLSVerify: options.skipTLSVerify,
          requestTimeoutMs: 3000,
        })) as Device
        return { ok: true, device: { ...device, ...probed, ip: probed.ip || device.ip, port: probed.port || device.port, manual: true } }
      } catch (e) {
        console.warn("[hook] ProbeManualDevice failed:", device.ip, e)
        return { ok: false, device }
      }
    }))

    const enriched = results.filter(r => r.ok).map(r => r.device)
    if (enriched.length > 0) {
      setManualDevices(prev => upsertDevices(prev, enriched))
    }
    const failed = parsed.length - enriched.length
    setStatus(`Added ${parsed.length} manual device${parsed.length === 1 ? '' : 's'} | info loaded: ${enriched.length}${failed ? `, failed: ${failed}` : ''}`)
    return parsed.length
  }, [])

  const removeManualDevice = useCallback((key: string) => {
    setManualDevices(prev => prev.filter(d => deviceKey(d) !== key))
    setSelectedMacs(prev => {
      const next = new Set(prev)
      next.delete(key)
      return next
    })
    setSelectedMac(current => current === key ? null : current)
    setStatus("Manual device removed")
  }, [])

  return {
    devices, selectedDevice, selectedMac, setSelectedMac,
    selectedMacs, batchDevices,
    addManualDevices, removeManualDevice,
    toggleSelect, selectAll, clearSelection, isSelected,
    scanning, listening, status, listenerStats,
    start, stop, scan, refresh,
  }
}

const MANUAL_DEVICES_KEY = 'ct-disc.manualDevices'

function mergeDevices(manual: Device[], discovered: Device[]) {
  const byKey = new Map<string, Device>()
  for (const device of manual) {
    const key = deviceKey(device)
    if (key) byKey.set(key, device)
  }
  for (const device of discovered) {
    const key = deviceKey(device)
    if (key) byKey.set(key, device)
  }
  return Array.from(byKey.values()).sort((a, b) => a.ip.localeCompare(b.ip))
}

function upsertDevices(prev: Device[], nextDevices: Device[]) {
  const byKey = new Map(prev.map(d => [deviceKey(d), d] as const))
  for (const device of nextDevices) {
    const key = deviceKey(device)
    if (key) byKey.set(key, device)
  }
  return Array.from(byKey.values()).sort((a, b) => a.ip.localeCompare(b.ip))
}

export function deviceKey(device: Device) {
  return device.ip || device.mac || device.sn || ''
}

function parseManualDevices(value: string, options: ManualProbeOptions): Device[] {
  const seen = new Set<string>()
  return value
    .split(/[\s,;]+/)
    .map(raw => parseManualDevice(raw, options))
    .filter((device): device is Device => Boolean(device))
    .filter(device => {
      const key = deviceKey(device)
      if (!key || seen.has(key)) return false
      seen.add(key)
      return true
    })
}

function parseManualDevice(value: string, options: ManualProbeOptions): Device | null {
  const raw = value.trim()
  if (!raw) return null

  let host = ''
  let port = options.apiPort || defaultPort(options.apiScheme)
  try {
    const parsed = new URL(raw.includes('://') ? raw : `http://${raw}`)
    host = parsed.hostname
    if (parsed.port) {
      port = Number(parsed.port)
    } else if (raw.includes('://')) {
      port = defaultPort(parsed.protocol.replace(':', '') as 'http' | 'https')
    }
  } catch {
    const withoutScheme = raw.replace(/^[a-z]+:\/\//i, '').split('/')[0]
    const parts = withoutScheme.split(':')
    host = parts[0]?.trim() || ''
    if (parts[1]) port = Number(parts[1]) || port
  }

  if (!host) return null
  const now = new Date().toLocaleString()
  return {
    sn: `manual-${host}`,
    product: 'Manual',
    ip: host,
    port,
    fw: '',
    caps: ['http'],
    hw: '',
    mac: '',
    online: true,
    lastSeen: now,
    firstSeen: now,
    manual: true,
  }
}

function defaultPort(scheme: 'http' | 'https') {
  return scheme === 'https' ? 443 : 8080
}

function loadManualDevices(): Device[] {
  try {
    const raw = localStorage.getItem(MANUAL_DEVICES_KEY)
    if (!raw) return []
    const parsed = JSON.parse(raw)
    if (!Array.isArray(parsed)) return []
    return parsed
      .map(item => normalizeStoredManualDevice(item))
      .filter((device): device is Device => Boolean(device))
  } catch {
    return []
  }
}

function saveManualDevices(devices: Device[]) {
  try {
    localStorage.setItem(MANUAL_DEVICES_KEY, JSON.stringify(devices.filter(d => d.manual)))
  } catch {
    // localStorage can be unavailable in restricted WebViews.
  }
}

function normalizeStoredManualDevice(item: Partial<Device> | null): Device | null {
  if (!item || !item.ip) return null
  return {
    sn: item.sn || `manual-${item.ip}`,
    product: item.product || 'Manual',
    ip: item.ip,
    port: Number(item.port || 8080),
    fw: item.fw || '',
    caps: Array.isArray(item.caps) ? item.caps : ['http'],
    hw: item.hw || '',
    mac: item.mac || '',
    online: true,
    lastSeen: item.lastSeen || '',
    firstSeen: item.firstSeen || '',
    manual: true,
  }
}
