import { useState, useEffect } from 'react'
import { useDevices } from './hooks/useDevices'
import { DeviceTable } from './components/DeviceTable'
import { DeviceDetail } from './components/DeviceDetail'
import { CommandDialog } from './components/CommandDialog'
import { SettingsPanel } from './components/SettingsPanel'
import { StatusBar } from './components/StatusBar'
import { NetworkConfigDialog } from './components/NetworkConfigDialog'
import { BatchToolbar } from './components/BatchToolbar'
import { RecordDialog } from './components/RecordDialog'
import { AddDeviceDialog } from './components/AddDeviceDialog'
import { GetNetworkInterfaces } from '../wailsjs/go/main/App'
import './style.css'

export default function App() {
  const {
    devices, selectedDevice, selectedMac, setSelectedMac,
    scanning, listening, status, listenerStats,
    start, scan, refresh,
    selectedMacs, batchDevices,
    addManualDevices, removeManualDevice,
    toggleSelect, selectAll, clearSelection,
  } = useDevices()

  const [ifaces, setIfaces] = useState<string[]>([])
  const [currentIface, setCurrentIface] = useState('')
  const [showSettings, setShowSettings] = useState(false)
  const [showCommand, setShowCommand] = useState(false)
  const [commandForBatch, setCommandForBatch] = useState(false)
  const [showNetwork, setShowNetwork] = useState(false)
  const [networkForBatch, setNetworkForBatch] = useState(false)
  const [showRecord, setShowRecord] = useState(false)
  const [recordForBatch, setRecordForBatch] = useState(false)
  const [showAddDevice, setShowAddDevice] = useState(false)
  const [showDetail, setShowDetail] = useState(false)

  useEffect(() => {
    GetNetworkInterfaces().then((list: string[]) => {
      setIfaces(list || [])
    }).catch((e: unknown) => console.error("[app] GetNetworkInterfaces error:", e))
  }, [])

  useEffect(() => {
    if (listening) return
    start(currentIface)
  }, [])

  const handleSelect = (mac: string) => {
    setSelectedMac(mac)
    setShowDetail(true)
  }

  const handleScan = async () => {
    if (!listening) {
      await start(currentIface)
    }
    await scan(currentIface)
  }

  const hasSelection = batchDevices.length > 0

  return (
    <div className="flex flex-col h-screen bg-white">
      {/* Header */}
      <div className="wails-drag flex items-center justify-between px-4 py-2.5 border-b bg-gray-900 text-white">
        <div className="flex items-center gap-3 wails-no-drag">
          <h1 className="text-base font-semibold">CT-Disc Discovery</h1>
          <select
            value={currentIface}
            onChange={e => setCurrentIface(e.target.value)}
            className="text-xs border border-gray-600 bg-gray-800 text-gray-200 rounded px-2 py-1 focus:outline-none focus:border-blue-500"
          >
            <option value="">All Interfaces</option>
            {ifaces.map(i => <option key={i} value={i}>{i}</option>)}
          </select>
        </div>
        <div className="flex items-center gap-2 wails-no-drag">
          <button
            onClick={handleScan}
            disabled={scanning}
            className="px-3 py-1.5 text-sm bg-blue-600 text-white rounded hover:bg-blue-700 disabled:opacity-50 transition-colors"
          >
            {scanning ? 'Scanning...' : 'Scan'}
          </button>
          <button
            onClick={() => setShowAddDevice(true)}
            className="px-3 py-1.5 text-sm border border-gray-600 text-gray-200 rounded hover:bg-gray-700 transition-colors"
          >
            Add Device
          </button>
          <button
            onClick={() => { setCommandForBatch(true); setShowCommand(true) }}
            disabled={!hasSelection && !selectedDevice}
            className="px-3 py-1.5 text-sm border border-gray-600 text-gray-200 rounded hover:bg-gray-700 disabled:opacity-40 transition-colors"
          >
            Send Command
          </button>
          <button
            onClick={() => { setNetworkForBatch(true); setShowNetwork(true) }}
            disabled={!hasSelection && !selectedDevice}
            className="px-3 py-1.5 text-sm border border-gray-600 text-gray-200 rounded hover:bg-gray-700 disabled:opacity-40 transition-colors"
          >
            Network Config
          </button>
          <button
            onClick={() => { setRecordForBatch(hasSelection); setShowRecord(true) }}
            className="px-3 py-1.5 text-sm border border-gray-600 text-gray-200 rounded hover:bg-gray-700 transition-colors"
          >
            Record Data
          </button>
          <button
            onClick={() => setShowSettings(true)}
            className="px-3 py-1.5 text-sm border border-gray-600 text-gray-200 rounded hover:bg-gray-700 transition-colors"
          >
            Settings
          </button>
        </div>
      </div>

      {/* Batch toolbar */}
      <BatchToolbar
        selectedDevices={batchDevices}
        onClearSelection={clearSelection}
        onBatchCommand={() => { setCommandForBatch(true); setShowCommand(true) }}
        onBatchNetwork={() => { setNetworkForBatch(true); setShowNetwork(true) }}
        onBatchRecord={() => { setRecordForBatch(true); setShowRecord(true) }}
      />

      {/* Status message */}
      <div className="px-4 py-1.5 bg-gray-50 border-b text-xs text-gray-600">
        {status}
      </div>

      {/* Main content — full width table */}
      <div className="flex-1 overflow-hidden">
        <DeviceTable
          devices={devices}
          selectedMac={selectedMac}
          onSelect={handleSelect}
          selectedMacs={selectedMacs}
          onToggleSelect={toggleSelect}
          onSelectAll={selectAll}
        />
      </div>

      {/* Status bar */}
      <StatusBar devices={devices} listening={listening} iface={currentIface} stats={listenerStats} />

      {/* Device Detail popup */}
      <DeviceDetail
        device={selectedDevice}
        show={showDetail}
        onClose={() => setShowDetail(false)}
        onCommand={() => { setShowDetail(false); setCommandForBatch(false); setShowCommand(true) }}
        onNetwork={() => { setShowDetail(false); setNetworkForBatch(false); setShowNetwork(true) }}
        onRemoveManual={() => {
          if (selectedDevice) removeManualDevice(selectedDevice.ip || selectedDevice.mac || selectedDevice.sn)
          setShowDetail(false)
        }}
      />

      {/* Dialogs */}
      {showCommand && (
        <CommandDialog
          device={selectedDevice}
          devices={commandForBatch && hasSelection ? batchDevices : undefined}
          onClose={() => setShowCommand(false)}
        />
      )}
      {showSettings && (
        <SettingsPanel onClose={() => setShowSettings(false)} />
      )}
      {showNetwork && (
        <NetworkConfigDialog
          device={selectedDevice}
          devices={networkForBatch && hasSelection ? batchDevices : undefined}
          onClose={() => setShowNetwork(false)}
        />
      )}
      {showRecord && (
        <RecordDialog
          device={selectedDevice}
          devices={recordForBatch && hasSelection ? batchDevices : undefined}
          onClose={() => setShowRecord(false)}
        />
      )}
      {showAddDevice && (
        <AddDeviceDialog
          onAdd={addManualDevices}
          onClose={() => setShowAddDevice(false)}
        />
      )}
    </div>
  )
}
