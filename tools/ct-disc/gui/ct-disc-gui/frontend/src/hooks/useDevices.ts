import { useState, useEffect, useCallback } from 'react'
import { EventsOn, EventsOff } from '../../wailsjs/runtime/runtime'
import {
  GetDevices,
  StartDiscovery,
  StopDiscovery,
  ScanDevices,
  Ping,
  GetListenerStats,
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
}

export interface ListenerStats {
  recvCount: number
  decodeErrs: number
  eventCount: number
  running: boolean
  ifaces: string[]
}

export function useDevices() {
  const [devices, setDevices] = useState<Device[]>([])
  const [selectedMacs, setSelectedMacs] = useState<Set<string>>(new Set())
  const [selectedMac, setSelectedMac] = useState<string | null>(null)
  const [scanning, setScanning] = useState(false)
  const [listening, setListening] = useState(false)
  const [status, setStatus] = useState("Initializing...")
  const [listenerStats, setListenerStats] = useState<ListenerStats | null>(null)

  const refresh = useCallback(async () => {
    try {
      const list = await GetDevices()
      setDevices(list || [])
    } catch (e: unknown) {
      setStatus("Failed to get devices: " + String(e))
    }
  }, [])

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
  const selectedDevice = devices.find(d => d.mac && d.mac === selectedMac) ||
    devices.find(d => d.sn === selectedMac) ||
    null

  // Batch selection helpers
  const toggleSelect = (mac: string) => {
    setSelectedMacs(prev => {
      const next = new Set(prev)
      if (next.has(mac)) {
        next.delete(mac)
      } else {
        next.add(mac)
      }
      return next
    })
  }

  const selectAll = () => {
    const onlineMacs = devices.filter(d => d.online && d.mac).map(d => d.mac!)
    if (onlineMacs.length === 0) return
    // Toggle: if all online are already selected, clear instead
    const allSelected = onlineMacs.every(mac => selectedMacs.has(mac))
    setSelectedMacs(allSelected ? new Set() : new Set(onlineMacs))
  }

  const clearSelection = () => {
    setSelectedMacs(new Set())
  }

  const isSelected = (mac: string) => selectedMacs.has(mac)

  const batchDevices = devices.filter(d => d.mac && selectedMacs.has(d.mac))

  return {
    devices, selectedDevice, selectedMac, setSelectedMac,
    selectedMacs, batchDevices,
    toggleSelect, selectAll, clearSelection, isSelected,
    scanning, listening, status, listenerStats,
    start, stop, scan, refresh,
  }
}
