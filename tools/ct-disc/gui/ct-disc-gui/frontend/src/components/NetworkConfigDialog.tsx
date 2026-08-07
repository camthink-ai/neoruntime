import { useState, useEffect, useRef } from 'react'
import type { Device } from '../hooks/useDevices'
import { GetDeviceNetworkConfig, SetDeviceNetworkConfig } from '../../wailsjs/go/main/App'

interface Props {
  device: Device | null
  devices?: Device[]
  onClose: () => void
}

export function NetworkConfigDialog({ device, devices, onClose }: Props) {
  const [mode, setMode] = useState('dhcp')
  const [ipAddress, setIpAddress] = useState('')
  const [subnetMask, setSubnetMask] = useState('255.255.255.0')
  const [gateway, setGateway] = useState('')
  const [dns1, setDns1] = useState('')
  const [dns2, setDns2] = useState('')
  const [configLoaded, setConfigLoaded] = useState(false)
  const [saving, setSaving] = useState(false)
  const [message, setMessage] = useState('')
  const [error, setError] = useState('')

  const loadedSn = useRef<string | null>(null)

  const targets = devices && devices.length > 0 ? devices : (device ? [device] : [])
  const primary = targets[0] || null
  const isBatch = targets.length > 1

  // Load config in background — form is always shown immediately
  useEffect(() => {
    if (!primary) return
    if (loadedSn.current === primary.sn) return
    loadedSn.current = primary.sn

    setConfigLoaded(false)
    setError('')
    setMessage('')

    console.log('[NetworkConfig] Fetching config for', primary.ip, primary.sn)
    GetDeviceNetworkConfig(primary.ip)
      .then((cfg) => {
        console.log('[NetworkConfig] Config loaded:', cfg)
        setMode(cfg.mode || 'dhcp')
        setIpAddress(cfg.ip_address || '')
        setSubnetMask(cfg.subnet_mask || '255.255.255.0')
        setGateway(cfg.gateway || '')
        setDns1(cfg.dns1 || '')
        setDns2(cfg.dns2 || '')
        setConfigLoaded(true)
      })
      .catch((err) => {
        console.warn('[NetworkConfig] Config load failed:', err)
        setConfigLoaded(true) // still show form, just with defaults
      })
  }, [primary?.sn])

  // Reset loadedSn when primary changes to null
  useEffect(() => {
    if (!primary) loadedSn.current = null
  }, [primary])

  if (targets.length === 0) return null

  const title = isBatch
    ? `Network Config — ${targets.length} devices`
    : `Network Config — ${primary?.sn}`

  const handleSave = (e: React.MouseEvent) => {
    e.preventDefault()
    e.stopPropagation()
    console.log('[NetworkConfig] handleSave clicked, targets:', targets.length, 'mode:', mode)

    setError('')
    setMessage('')
    setSaving(true)

    // Use an async IIFE so the click handler doesn't return a promise
    ;(async () => {
      try {
        const results: string[] = []
        for (const d of targets) {
          const cfg = {
            interface: 'eth0',
            mode,
            ip_address: mode === 'static' ? ipAddress : '',
            subnet_mask: mode === 'static' ? subnetMask : '',
            gateway: mode === 'static' ? gateway : '',
            dns1,
            dns2,
            sn: d.sn || '',
            mac: d.mac || '',
          }
          const jsonStr = JSON.stringify(cfg)
          console.log('[NetworkConfig] Calling SetDeviceNetworkConfig ip=', d.ip, 'cfg=', jsonStr)
          try {
            const result = await SetDeviceNetworkConfig(d.ip, jsonStr)
            console.log('[NetworkConfig] OK:', d.sn, result)
            results.push(`${d.sn}: ${result}`)
          } catch (e) {
            console.error('[NetworkConfig] FAIL:', d.sn, e)
            results.push(`${d.sn}: ERROR — ${String(e)}`)
          }
        }
        setMessage(results.join('\n'))
      } catch (e: unknown) {
        console.error('[NetworkConfig] Fatal:', e)
        setError(String(e))
      } finally {
        setSaving(false)
      }
    })()
  }

  return (
    <div className="fixed inset-0 bg-black/30 flex items-center justify-center z-50" onClick={onClose}>
      <div className="bg-white rounded-lg shadow-xl w-[460px] p-6" onClick={e => e.stopPropagation()}>
        <div className="flex items-center justify-between mb-4">
          <h3 className="text-lg font-semibold text-gray-800">{title}</h3>
          <button type="button" onClick={onClose} className="text-gray-400 hover:text-gray-600 text-xl leading-none">&times;</button>
        </div>

        {isBatch && (
          <div className="text-xs text-gray-500 mb-3 bg-gray-50 rounded p-2 font-mono max-h-16 overflow-auto">
            {targets.map(d => d.sn).join(', ')}
          </div>
        )}
        {!isBatch && primary && (
          <div className="text-xs text-gray-500 mb-3">
            Device: {primary.product} | IP: {primary.ip}
          </div>
        )}

        {/* Always render the form */}
        <div className="space-y-3">
          {!configLoaded && (
            <div className="text-xs text-gray-400 italic">Loading current config...</div>
          )}

          <div>
            <label className="text-sm text-gray-600">Mode</label>
            <div className="flex gap-2 mt-1">
              <button type="button"
                onClick={() => setMode('dhcp')}
                className={`px-3 py-1.5 text-sm rounded transition-colors ${mode === 'dhcp' ? 'bg-blue-600 text-white' : 'bg-gray-100 text-gray-700'}`}
              >
                DHCP
              </button>
              <button type="button"
                onClick={() => setMode('static')}
                className={`px-3 py-1.5 text-sm rounded transition-colors ${mode === 'static' ? 'bg-blue-600 text-white' : 'bg-gray-100 text-gray-700'}`}
              >
                Static IP
              </button>
            </div>
          </div>

          {mode === 'static' && (
            <>
              <div>
                <label className="text-sm text-gray-600">IP Address</label>
                <input
                  value={ipAddress}
                  onChange={e => setIpAddress(e.target.value)}
                  className="w-full mt-1 p-2 border rounded text-sm font-mono"
                  placeholder="192.168.1.100"
                />
              </div>
              <div>
                <label className="text-sm text-gray-600">Subnet Mask</label>
                <input
                  value={subnetMask}
                  onChange={e => setSubnetMask(e.target.value)}
                  className="w-full mt-1 p-2 border rounded text-sm font-mono"
                  placeholder="255.255.255.0"
                />
              </div>
              <div>
                <label className="text-sm text-gray-600">Gateway</label>
                <input
                  value={gateway}
                  onChange={e => setGateway(e.target.value)}
                  className="w-full mt-1 p-2 border rounded text-sm font-mono"
                  placeholder="192.168.1.1"
                />
              </div>
            </>
          )}

          <div>
            <label className="text-sm text-gray-600">DNS 1</label>
            <input
              value={dns1}
              onChange={e => setDns1(e.target.value)}
              className="w-full mt-1 p-2 border rounded text-sm font-mono"
              placeholder="8.8.8.8"
            />
          </div>
          <div>
            <label className="text-sm text-gray-600">DNS 2</label>
            <input
              value={dns2}
              onChange={e => setDns2(e.target.value)}
              className="w-full mt-1 p-2 border rounded text-sm font-mono"
              placeholder="8.8.4.4"
            />
          </div>

          {error && <div className="text-red-600 text-sm bg-red-50 p-2 rounded">{error}</div>}
          {message && <div className="text-green-600 text-sm bg-green-50 p-2 rounded whitespace-pre-wrap font-mono">{message}</div>}

          <div className="flex justify-end gap-2 pt-2">
            <button type="button" onClick={onClose} className="px-4 py-2 text-sm text-gray-600 hover:text-gray-800">
              Close
            </button>
            <button type="button"
              onClick={handleSave}
              disabled={saving || (mode === 'static' && !ipAddress)}
              className="px-4 py-2 text-sm bg-blue-600 text-white rounded hover:bg-blue-700 disabled:opacity-50 transition-colors"
            >
              {saving ? 'Applying...' : 'Apply'}
            </button>
          </div>
        </div>
      </div>
    </div>
  )
}
