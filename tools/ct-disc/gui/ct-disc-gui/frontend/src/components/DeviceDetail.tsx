import type { Device } from '../hooks/useDevices'
import { OpenInBrowser } from '../../wailsjs/go/main/App'

interface Props {
  device: Device | null
  show: boolean
  onClose: () => void
  onCommand: () => void
  onNetwork: () => void
}

export function DeviceDetail({ device, show, onClose, onCommand, onNetwork }: Props) {
  if (!show || !device) return null

  const fields = [
    { label: 'Serial Number', value: device.sn },
    { label: 'Product', value: device.product },
    { label: 'Firmware', value: device.fw },
    { label: 'Hardware', value: device.hw || '-' },
    { label: 'IP Address', value: device.ip },
    { label: 'Port', value: String(device.port) },
    { label: 'MAC Address', value: device.mac || '-' },
    { label: 'First Seen', value: device.firstSeen },
    { label: 'Last Seen', value: device.lastSeen },
  ]

  return (
    <div className="fixed inset-0 bg-black/30 flex items-center justify-center z-40" onClick={onClose}>
      <div className="bg-white rounded-lg shadow-xl w-[520px] max-h-[80vh] overflow-auto" onClick={e => e.stopPropagation()}>
        {/* Header */}
        <div className="flex items-center gap-3 px-5 py-4 border-b sticky top-0 bg-white z-10">
          <span className={`inline-block w-3 h-3 rounded-full ${device.online ? 'bg-green-500 status-pulse' : 'bg-gray-300'}`} />
          <div className="flex-1">
            <h2 className="text-lg font-semibold text-gray-800">{device.sn}</h2>
            <p className="text-xs text-gray-500">{device.product}</p>
          </div>
          <span className={`px-2 py-0.5 text-xs rounded-full font-medium ${device.online ? 'bg-green-50 text-green-700 border border-green-200' : 'bg-gray-50 text-gray-500 border border-gray-200'}`}>
            {device.online ? 'Online' : 'Offline'}
          </span>
          <button onClick={onClose} className="ml-2 text-gray-400 hover:text-gray-600 text-xl leading-none">&times;</button>
        </div>

        <div className="p-5 space-y-4">
          {/* Action buttons */}
          <div className="flex gap-2 flex-wrap">
            <button
              onClick={() => OpenInBrowser(`http://${device.ip}:${device.port}`)}
              disabled={!device.online}
              className="inline-flex items-center gap-1.5 px-3 py-1.5 text-xs font-medium bg-blue-600 text-white rounded-md hover:bg-blue-700 disabled:opacity-40 disabled:cursor-not-allowed transition-colors"
            >
              <svg className="w-3.5 h-3.5" fill="none" viewBox="0 0 24 24" stroke="currentColor"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M21 12a9 9 0 01-9 9m9-9a9 9 0 00-9-9m9 9H3m9 9a9 9 0 01-9-9m9 9c1.657 0 3-4.03 3-9s-1.343-9-3-9m0 18c-1.657 0-3-4.03-3-9s1.343-9 3-9m-9 9a9 9 0 019-9" /></svg>
              Open Web UI
            </button>
            <button
              onClick={onNetwork}
              disabled={!device.online}
              className="inline-flex items-center gap-1.5 px-3 py-1.5 text-xs font-medium border border-gray-300 text-gray-700 rounded-md hover:bg-gray-50 disabled:opacity-40 disabled:cursor-not-allowed transition-colors"
            >
              <svg className="w-3.5 h-3.5" fill="none" viewBox="0 0 24 24" stroke="currentColor"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M12 6V4m0 2a2 2 0 100 4m0-4a2 2 0 110 4m-6 8a2 2 0 100-4m0 4a2 2 0 110-4m0 4v2m0-6V4m6 6v10m6-2a2 2 0 100-4m0 4a2 2 0 110-4m0 4v2m0-6V4" /></svg>
              Network Config
            </button>
            <button
              onClick={onCommand}
              disabled={!device.online}
              className="inline-flex items-center gap-1.5 px-3 py-1.5 text-xs font-medium border border-gray-300 text-gray-700 rounded-md hover:bg-gray-50 disabled:opacity-40 disabled:cursor-not-allowed transition-colors"
            >
              <svg className="w-3.5 h-3.5" fill="none" viewBox="0 0 24 24" stroke="currentColor"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M8 9l3 3-3 3m5 0h3M5 20h14a2 2 0 002-2V6a2 2 0 00-2-2H5a2 2 0 00-2 2v12a2 2 0 002 2z" /></svg>
              Send Command
            </button>
          </div>

          {/* Details card */}
          <div className="detail-card p-4">
            <h3 className="text-xs font-semibold text-gray-400 uppercase tracking-wider mb-3">Device Info</h3>
            <div className="grid grid-cols-2 gap-x-6 gap-y-3 text-sm">
              {fields.map(f => (
                <div key={f.label}>
                  <span className="text-xs text-gray-400">{f.label}</span>
                  <div className="font-mono text-sm text-gray-700 mt-0.5">{f.value}</div>
                </div>
              ))}
            </div>
          </div>

          {/* Capabilities */}
          <div className="detail-card p-4">
            <h3 className="text-xs font-semibold text-gray-400 uppercase tracking-wider mb-2">Capabilities</h3>
            <div className="flex flex-wrap gap-1.5">
              {device.caps.map(c => (
                <span key={c} className="px-2.5 py-1 bg-blue-50 text-blue-700 text-xs rounded-full font-medium border border-blue-100">
                  {c}
                </span>
              ))}
            </div>
          </div>
        </div>
      </div>
    </div>
  )
}
