import { useState } from 'react'
import type { Device } from '../hooks/useDevices'
import { SendCommand } from '../../wailsjs/go/main/App'

interface Props {
  device: Device | null
  devices?: Device[]
  onClose: () => void
}

const PRESET_COMMANDS = ['reboot', 'info', 'ping', 'config']

export function CommandDialog({ device, devices, onClose }: Props) {
  const [cmd, setCmd] = useState('ping')
  const [payload, setPayload] = useState('{}')
  const [response, setResponse] = useState('')
  const [error, setError] = useState('')
  const [sending, setSending] = useState(false)

  const targets = devices && devices.length > 0 ? devices : (device ? [device] : [])
  if (targets.length === 0) return null

  const isBatch = targets.length > 1
  const title = isBatch
    ? `Send Command — ${targets.length} devices`
    : `Send Command — ${targets[0].sn}`

  const handleSend = async () => {
    setSending(true)
    setError('')
    setResponse('')
    try {
      const results: string[] = []
      for (const d of targets) {
        try {
          const resp = await SendCommand(d.sn, cmd, payload)
          results.push(`${d.sn}: ${resp}`)
        } catch (e) {
          results.push(`${d.sn}: ERROR — ${String(e)}`)
        }
      }
      setResponse(results.join('\n'))
    } catch (e) {
      setError(String(e))
    } finally {
      setSending(false)
    }
  }

  return (
    <div className="fixed inset-0 bg-black/30 flex items-center justify-center z-50" onClick={onClose}>
      <div className="bg-white rounded-lg shadow-xl w-[480px] p-6" onClick={e => e.stopPropagation()}>
        <div className="flex items-center justify-between mb-4">
          <h3 className="text-lg font-semibold text-gray-800">{title}</h3>
          <button onClick={onClose} className="text-gray-400 hover:text-gray-600 text-xl leading-none">&times;</button>
        </div>

        {isBatch && (
          <div className="text-xs text-gray-500 mb-3 bg-gray-50 rounded p-2 font-mono max-h-16 overflow-auto">
            {targets.map(d => d.sn).join(', ')}
          </div>
        )}

        <div className="space-y-3">
          <div>
            <label className="text-sm text-gray-600">Command</label>
            <div className="flex gap-1 mt-1 flex-wrap">
              {PRESET_COMMANDS.map(c => (
                <button
                  key={c}
                  onClick={() => setCmd(c)}
                  className={`px-2.5 py-1 text-xs rounded transition-colors ${
                    cmd === c ? 'bg-blue-600 text-white' : 'bg-gray-100 text-gray-700 hover:bg-gray-200'
                  }`}
                >
                  {c}
                </button>
              ))}
            </div>
          </div>

          <div>
            <label className="text-sm text-gray-600">Payload (JSON)</label>
            <textarea
              value={payload}
              onChange={e => setPayload(e.target.value)}
              className="w-full mt-1 p-2 border rounded text-sm font-mono h-20 resize-none"
            />
          </div>

          {error && <div className="text-red-600 text-sm bg-red-50 p-2 rounded">{error}</div>}
          {response && (
            <div className="bg-gray-50 p-2 rounded text-sm font-mono whitespace-pre-wrap max-h-40 overflow-auto">
              {response}
            </div>
          )}

          <div className="flex justify-end gap-2 pt-2">
            <button onClick={onClose} className="px-4 py-2 text-sm text-gray-600 hover:text-gray-800">
              Close
            </button>
            <button
              onClick={handleSend}
              disabled={sending}
              className="px-4 py-2 text-sm bg-blue-600 text-white rounded hover:bg-blue-700 disabled:opacity-50 transition-colors"
            >
              {sending ? 'Sending...' : 'Send'}
            </button>
          </div>
        </div>
      </div>
    </div>
  )
}
