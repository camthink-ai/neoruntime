import { useState, useEffect } from 'react'
import { GetSettings, SaveSettings } from '../../wailsjs/go/main/App'

interface Props {
  onClose: () => void
}

export function SettingsPanel({ onClose }: Props) {
  const [broker, setBroker] = useState('tcp://localhost:1883')
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [saved, setSaved] = useState(false)

  useEffect(() => {
    GetSettings().then((s: any) => {
      setBroker(s.mqttBroker || 'tcp://localhost:1883')
      setUsername(s.mqttUser || '')
      setPassword(s.mqttPass || '')
    })
  }, [])

  const handleSave = async () => {
    await SaveSettings(JSON.stringify({ mqttBroker: broker, mqttUser: username, mqttPass: password }))
    setSaved(true)
    setTimeout(() => setSaved(false), 2000)
  }

  return (
    <div className="fixed inset-0 bg-black/30 flex items-center justify-center z-50" onClick={onClose}>
      <div className="bg-white rounded-lg shadow-xl w-[400px] p-6" onClick={e => e.stopPropagation()}>
        <div className="flex items-center justify-between mb-4">
          <h3 className="text-lg font-semibold text-gray-800">Settings</h3>
          <button onClick={onClose} className="text-gray-400 hover:text-gray-600 text-xl leading-none">&times;</button>
        </div>

        <div className="space-y-3">
          <div>
            <label className="text-sm text-gray-600">MQTT Broker</label>
            <input
              value={broker}
              onChange={e => setBroker(e.target.value)}
              className="w-full mt-1 p-2 border rounded text-sm font-mono"
              placeholder="tcp://broker:1883"
            />
          </div>
          <div>
            <label className="text-sm text-gray-600">MQTT Username</label>
            <input
              value={username}
              onChange={e => setUsername(e.target.value)}
              className="w-full mt-1 p-2 border rounded text-sm"
            />
          </div>
          <div>
            <label className="text-sm text-gray-600">MQTT Password</label>
            <input
              type="password"
              value={password}
              onChange={e => setPassword(e.target.value)}
              className="w-full mt-1 p-2 border rounded text-sm"
            />
          </div>

          {saved && <div className="text-green-600 text-sm">Settings saved</div>}

          <div className="flex justify-end gap-2 pt-2">
            <button onClick={onClose} className="px-4 py-2 text-sm text-gray-600 hover:text-gray-800">
              Close
            </button>
            <button onClick={handleSave} className="px-4 py-2 text-sm bg-blue-600 text-white rounded hover:bg-blue-700 transition-colors">
              Save
            </button>
          </div>
        </div>
      </div>
    </div>
  )
}
