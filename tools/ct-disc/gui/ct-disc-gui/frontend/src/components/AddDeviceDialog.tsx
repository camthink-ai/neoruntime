import { useState } from 'react'
import type { ManualProbeOptions } from '../hooks/useDevices'

interface Props {
  onClose: () => void
  onAdd: (value: string, options: ManualProbeOptions) => Promise<number>
}

export function AddDeviceDialog({ onClose, onAdd }: Props) {
  const [value, setValue] = useState('')
  const [apiScheme, setApiScheme] = useState<'http' | 'https'>('https')
  const [apiPort, setApiPort] = useState(443)
  const [username, setUsername] = useState('')
  const [token, setToken] = useState('')
  const [skipTLSVerify, setSkipTLSVerify] = useState(true)
  const [adding, setAdding] = useState(false)
  const [error, setError] = useState('')

  const submit = async () => {
    setError('')
    setAdding(true)
    try {
      const count = await onAdd(value, {
        apiScheme,
        apiPort,
        username,
        token,
        skipTLSVerify,
      })
      if (count === 0) {
        setError('Enter at least one IP address or URL.')
        return
      }
      onClose()
    } catch (e) {
      setError(String(e))
    } finally {
      setAdding(false)
    }
  }

  return (
    <div className="fixed inset-0 bg-black/30 flex items-center justify-center z-50" onClick={onClose}>
      <div className="bg-white rounded-lg shadow-xl w-[520px] p-5" onClick={e => e.stopPropagation()}>
        <div className="flex items-center justify-between mb-4">
          <h3 className="text-lg font-semibold text-gray-800">Add Device</h3>
          <button onClick={onClose} className="text-gray-400 hover:text-gray-600 text-xl leading-none">&times;</button>
        </div>

        <label className="text-sm text-gray-600">IP Addresses or URLs</label>
        <textarea
          value={value}
          autoFocus
          disabled={adding}
          onChange={e => setValue(e.target.value)}
          placeholder="192.168.1.100&#10;https://192.168.1.101"
          className="w-full mt-1 p-2 border rounded text-sm font-mono h-28 resize-none focus:outline-none focus:ring-1 focus:ring-blue-400"
        />

        <div className="grid grid-cols-2 gap-3 mt-3">
          <div>
            <label className="text-sm text-gray-600">Protocol</label>
            <select
              value={apiScheme}
              disabled={adding}
              onChange={e => {
                const next = e.target.value as 'http' | 'https'
                setApiScheme(next)
                if (next === 'https' && apiPort === 8080) setApiPort(443)
                if (next === 'http' && apiPort === 443) setApiPort(8080)
              }}
              className="w-full mt-1 p-2 border rounded text-sm"
            >
              <option value="https">HTTPS</option>
              <option value="http">HTTP</option>
            </select>
          </div>
          <div>
            <label className="text-sm text-gray-600">API Port</label>
            <input
              type="number"
              min={1}
              value={apiPort}
              disabled={adding}
              onChange={e => setApiPort(Math.max(1, Number(e.target.value || 1)))}
              className="w-full mt-1 p-2 border rounded text-sm"
            />
          </div>
          <div>
            <label className="text-sm text-gray-600">Username</label>
            <input
              value={username}
              disabled={adding}
              onChange={e => setUsername(e.target.value)}
              className="w-full mt-1 p-2 border rounded text-sm"
            />
          </div>
          <div>
            <label className="text-sm text-gray-600">Token</label>
            <input
              type="password"
              value={token}
              disabled={adding}
              onChange={e => setToken(e.target.value)}
              className="w-full mt-1 p-2 border rounded text-sm"
            />
          </div>
        </div>

        <label className="flex items-center gap-2 mt-3 text-sm text-gray-600">
          <input
            type="checkbox"
            checked={skipTLSVerify}
            disabled={adding}
            onChange={e => setSkipTLSVerify(e.target.checked)}
          />
          Skip HTTPS certificate verification
        </label>

        {error && (
          <div className="mt-3 text-red-600 text-sm bg-red-50 p-2 rounded">
            {error}
          </div>
        )}

        <div className="flex justify-end gap-2 pt-4">
          <button onClick={onClose} disabled={adding} className="px-4 py-2 text-sm text-gray-600 hover:text-gray-800 disabled:opacity-50">
            Cancel
          </button>
          <button onClick={submit} disabled={adding} className="px-4 py-2 text-sm bg-blue-600 text-white rounded hover:bg-blue-700 disabled:opacity-50 transition-colors">
            {adding ? 'Adding...' : 'Add'}
          </button>
        </div>
      </div>
    </div>
  )
}
