import { useEffect, useMemo, useState } from 'react'
import { EventsOff, EventsOn } from '../../wailsjs/runtime/runtime'
import {
  ChooseRecordFile,
  GetDefaultRecordPath,
  GetRecordHistory,
  GetRecordStatus,
  StartMetricsRecording,
  StopMetricsRecording,
} from '../../wailsjs/go/main/App'
import type { Device } from '../hooks/useDevices'

interface RecordStatus {
  running: boolean
  outputPath: string
  format: string
  startedAt: string
  stoppedAt: string
  lastSampleAt: string
  lastError: string
  samplesWritten: number
  recordsWritten: number
  targetCount: number
}

interface MetricRecord {
  timestamp: string
  unix_ms: number
  sn?: string
  mac?: string
  product?: string
  ip?: string
  api_url: string
  online: boolean
  metrics_ok: boolean
  error?: string
  cpu_percent: number
  memory_percent: number
  disk_percent: number
  npu_percent: number
  temp_cpu: number
  temp_npu: number
  temp_board: number
  latency_ms: number
}

interface Props {
  device: Device | null
  devices?: Device[]
  onClose: () => void
}

const emptyStatus: RecordStatus = {
  running: false,
  outputPath: '',
  format: 'csv',
  startedAt: '',
  stoppedAt: '',
  lastSampleAt: '',
  lastError: '',
  samplesWritten: 0,
  recordsWritten: 0,
  targetCount: 0,
}

export function RecordDialog({ device, devices, onClose }: Props) {
  const [format, setFormat] = useState<'csv' | 'jsonl'>('csv')
  const [outputPath, setOutputPath] = useState('')
  const [apiScheme, setApiScheme] = useState<'http' | 'https'>('https')
  const [apiPort, setApiPort] = useState(443)
  const [oneFilePerDevice, setOneFilePerDevice] = useState(true)
  const [manualHosts, setManualHosts] = useState('')
  const [intervalSeconds, setIntervalSeconds] = useState(5)
  const [samples, setSamples] = useState(0)
  const [durationMinutes, setDurationMinutes] = useState(0)
  const [username, setUsername] = useState('')
  const [token, setToken] = useState('')
  const [skipTLSVerify, setSkipTLSVerify] = useState(true)
  const [status, setStatus] = useState<RecordStatus>(emptyStatus)
  const [history, setHistory] = useState<MetricRecord[]>([])
  const [error, setError] = useState('')

  const targets = useMemo(() => {
    const selected = devices && devices.length > 0 ? devices : (device ? [device] : [])
    const selectedTargets = selected.filter(d => d.online && d.ip)
    const manualTargets = parseManualDevices(manualHosts)
    const byKey = new Map<string, Device>()
    for (const target of [...selectedTargets, ...manualTargets]) {
      const key = deviceKey(target)
      if (key) byKey.set(key, target)
    }
    return Array.from(byKey.values())
  }, [device, devices, manualHosts])

  useEffect(() => {
    GetDefaultRecordPath(format).then(setOutputPath).catch(() => {})
  }, [format])

  useEffect(() => {
    GetRecordStatus().then((s: RecordStatus) => setStatus(s || emptyStatus)).catch(() => {})
    GetRecordHistory().then((list: MetricRecord[]) => setHistory(list || [])).catch(() => {})
    EventsOn('record:status', (s: RecordStatus) => setStatus(s || emptyStatus))
    EventsOn('record:sample', (records: MetricRecord[]) => {
      setHistory(prev => [...prev, ...(records || [])].slice(-5000))
    })
    return () => {
      EventsOff('record:status')
      EventsOff('record:sample')
    }
  }, [])

  const isBatch = targets.length > 1
  const title = isBatch
    ? `Record Data — ${targets.length} devices`
    : `Record Data${targets.length === 1 ? ` — ${targets[0].sn || targets[0].ip}` : ''}`
  const targetKeys = new Set(targets.map(deviceKey))
  const trendGroups = groupHistory(history.filter(r => targetKeys.has(recordKey(r)) && r.metrics_ok))

  const chooseFile = async () => {
    try {
      const selected = await ChooseRecordFile(outputPath)
      if (selected) setOutputPath(selected)
    } catch (e) {
      setError(String(e))
    }
  }

  const start = async () => {
    setError('')
    if (targets.length === 0) {
      setError('Select a device or enter at least one IP address.')
      return
    }
    try {
      const next = await StartMetricsRecording(JSON.stringify({
        devices: targets,
        outputPath,
        format,
        apiScheme,
        apiPort,
        oneFilePerDevice,
        intervalSeconds,
        samples,
        durationMinutes,
        username,
        token,
        skipTLSVerify,
        requestTimeoutMs: 3000,
      }))
      setStatus(next as RecordStatus)
    } catch (e) {
      setError(String(e))
    }
  }

  const stop = async () => {
    setError('')
    try {
      const next = await StopMetricsRecording()
      setStatus(next as RecordStatus)
    } catch (e) {
      setError(String(e))
    }
  }

  return (
    <div className="fixed inset-0 bg-black/30 flex items-center justify-center z-50" onClick={onClose}>
      <div className="bg-white rounded-lg shadow-xl w-[860px] max-h-[90vh] overflow-y-auto p-6" onClick={e => e.stopPropagation()}>
        <div className="flex items-center justify-between mb-4">
          <h3 className="text-lg font-semibold text-gray-800">{title}</h3>
          <button onClick={onClose} className="text-gray-400 hover:text-gray-600 text-xl leading-none">&times;</button>
        </div>

        <div className="text-xs text-gray-500 mb-3 bg-gray-50 rounded p-2 font-mono max-h-20 overflow-auto">
          {targets.length > 0
            ? targets.map(d => `${d.sn || d.mac || d.ip} (${apiScheme}://${d.ip}${apiPort ? `:${apiPort}` : ''})`).join(', ')
            : 'No selected devices. Enter IP addresses below to record manually.'}
        </div>

        <div className="mb-3">
          <label className="text-sm text-gray-600">Manual IP Addresses</label>
          <textarea
            value={manualHosts}
            disabled={status.running}
            onChange={e => setManualHosts(e.target.value)}
            placeholder="192.168.1.100&#10;192.168.1.101"
            className="w-full mt-1 p-2 border rounded text-sm font-mono h-16 resize-none"
          />
        </div>

        <div className="grid grid-cols-2 gap-3">
          <div>
            <label className="text-sm text-gray-600">Format</label>
            <select
              value={format}
              onChange={e => setFormat(e.target.value as 'csv' | 'jsonl')}
              disabled={status.running}
              className="w-full mt-1 p-2 border rounded text-sm"
            >
              <option value="csv">CSV</option>
              <option value="jsonl">JSON Lines</option>
            </select>
          </div>
          <div>
            <label className="text-sm text-gray-600">Protocol</label>
            <select
              value={apiScheme}
              onChange={e => {
                const next = e.target.value as 'http' | 'https'
                setApiScheme(next)
                if (next === 'https' && apiPort === 8080) setApiPort(443)
                if (next === 'http' && apiPort === 443) setApiPort(8080)
              }}
              disabled={status.running}
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
              min={0}
              value={apiPort}
              disabled={status.running}
              onChange={e => setApiPort(Math.max(0, Number(e.target.value || 0)))}
              className="w-full mt-1 p-2 border rounded text-sm"
            />
          </div>
          <div>
            <label className="text-sm text-gray-600">Interval Seconds</label>
            <input
              type="number"
              min={1}
              value={intervalSeconds}
              disabled={status.running}
              onChange={e => setIntervalSeconds(Math.max(1, Number(e.target.value || 1)))}
              className="w-full mt-1 p-2 border rounded text-sm"
            />
          </div>
          <div>
            <label className="text-sm text-gray-600">Samples</label>
            <input
              type="number"
              min={0}
              value={samples}
              disabled={status.running}
              onChange={e => setSamples(Math.max(0, Number(e.target.value || 0)))}
              className="w-full mt-1 p-2 border rounded text-sm"
            />
          </div>
          <div>
            <label className="text-sm text-gray-600">Duration Minutes</label>
            <input
              type="number"
              min={0}
              value={durationMinutes}
              disabled={status.running}
              onChange={e => setDurationMinutes(Math.max(0, Number(e.target.value || 0)))}
              className="w-full mt-1 p-2 border rounded text-sm"
            />
          </div>
        </div>

        <div className="mt-3">
          <label className="text-sm text-gray-600">{oneFilePerDevice ? 'Base Output File' : 'Output File'}</label>
          <div className="flex gap-2 mt-1">
            <input
              value={outputPath}
              disabled={status.running}
              onChange={e => setOutputPath(e.target.value)}
              className="flex-1 p-2 border rounded text-sm font-mono min-w-0"
            />
            <button
              onClick={chooseFile}
              disabled={status.running}
              className="px-3 py-2 text-sm border rounded hover:bg-gray-50 disabled:opacity-50"
            >
              Browse
            </button>
          </div>
        </div>

        <label className="flex items-center gap-2 mt-3 text-sm text-gray-600">
          <input
            type="checkbox"
            checked={oneFilePerDevice}
            disabled={status.running}
            onChange={e => setOneFilePerDevice(e.target.checked)}
          />
          One file per device
        </label>

        <div className="grid grid-cols-2 gap-3 mt-3">
          <div>
            <label className="text-sm text-gray-600">Username</label>
            <input
              value={username}
              disabled={status.running}
              onChange={e => setUsername(e.target.value)}
              className="w-full mt-1 p-2 border rounded text-sm"
            />
          </div>
          <div>
            <label className="text-sm text-gray-600">Token</label>
            <input
              type="password"
              value={token}
              disabled={status.running}
              onChange={e => setToken(e.target.value)}
              className="w-full mt-1 p-2 border rounded text-sm"
            />
          </div>
        </div>

        <label className="flex items-center gap-2 mt-3 text-sm text-gray-600">
          <input
            type="checkbox"
            checked={skipTLSVerify}
            disabled={status.running}
            onChange={e => setSkipTLSVerify(e.target.checked)}
          />
          Skip HTTPS certificate verification
        </label>

        {(error || status.lastError) && (
          <div className="mt-3 text-red-600 text-sm bg-red-50 p-2 rounded break-words">
            {error || status.lastError}
          </div>
        )}

        <div className="mt-3 bg-gray-50 p-3 rounded text-sm text-gray-700 grid grid-cols-2 gap-2">
          <div>State: <span className={status.running ? 'text-green-600' : 'text-gray-500'}>{status.running ? 'Recording' : 'Stopped'}</span></div>
          <div>Targets: {status.targetCount || targets.length}</div>
          <div>Samples: {status.samplesWritten}</div>
          <div>Records: {status.recordsWritten}</div>
          <div className="col-span-2 truncate" title={status.outputPath || outputPath}>
            File: {oneFilePerDevice ? `${outputPath.replace(/\.[^/.]+$/, '')}_<ip>${outputPath.match(/\.[^/.]+$/)?.[0] || ''}` : (status.outputPath || outputPath)}
          </div>
          {status.lastSampleAt && <div className="col-span-2">Last sample: {new Date(status.lastSampleAt).toLocaleString()}</div>}
        </div>

        <div className="mt-4">
          <div className="flex items-center justify-between mb-2">
            <h4 className="text-sm font-semibold text-gray-800">Trend Charts</h4>
            <span className="text-xs text-gray-400">{trendGroups.reduce((sum, group) => sum + group.records.length, 0)} sample records</span>
          </div>
          {trendGroups.length === 0 ? (
            <div className="border border-dashed rounded p-6 text-center text-sm text-gray-400">
              Start recording to generate per-device trend charts.
            </div>
          ) : (
            <div className="space-y-3">
              {trendGroups.map(group => (
                <DeviceTrend key={group.key} label={group.label} records={group.records} />
              ))}
            </div>
          )}
        </div>

        <div className="flex justify-end gap-2 pt-4">
          <button onClick={onClose} className="px-4 py-2 text-sm text-gray-600 hover:text-gray-800">
            Close
          </button>
          {status.running ? (
            <button onClick={stop} className="px-4 py-2 text-sm bg-red-600 text-white rounded hover:bg-red-700 transition-colors">
              Stop Recording
            </button>
          ) : (
            <button
              onClick={start}
              disabled={targets.length === 0}
              className="px-4 py-2 text-sm bg-blue-600 text-white rounded hover:bg-blue-700 disabled:opacity-50 transition-colors"
            >
              Start Recording
            </button>
          )}
        </div>
      </div>
    </div>
  )
}

function parseManualDevices(value: string): Device[] {
  return value
    .split(/[\s,;]+/)
    .map(normalizeManualHost)
    .filter((ip): ip is string => Boolean(ip))
    .map(ip => ({
      sn: '',
      product: 'Manual',
      ip,
      port: 0,
      fw: '',
      caps: [],
      hw: '',
      mac: '',
      online: true,
      lastSeen: '',
      firstSeen: '',
    }))
}

function normalizeManualHost(value: string) {
  const raw = value.trim()
  if (!raw) return ''
  try {
    const parsed = new URL(raw.includes('://') ? raw : `http://${raw}`)
    return parsed.hostname
  } catch {
    return raw.replace(/^[a-z]+:\/\//i, '').split('/')[0].split(':')[0].trim()
  }
}

function deviceKey(d: Device) {
  if (d.ip) return d.ip
  if (d.mac) return d.mac
  return d.sn || ''
}

function recordKey(r: MetricRecord) {
  return r.ip || r.api_url || r.mac || r.sn || ''
}

function groupHistory(records: MetricRecord[]) {
  const map = new Map<string, { key: string; label: string; records: MetricRecord[] }>()
  for (const record of records) {
    const key = recordKey(record)
    const label = recordLabel(record)
    if (!map.has(key)) map.set(key, { key, label, records: [] })
    map.get(key)!.records.push(record)
  }
  return Array.from(map.values())
    .map(group => ({ ...group, records: group.records.slice(-120) }))
    .sort((a, b) => a.label.localeCompare(b.label))
}

function recordLabel(record: MetricRecord) {
  if (record.ip && record.sn) return `${record.ip} (${record.sn})`
  return record.ip || record.api_url || record.sn || record.mac || 'unknown'
}

function DeviceTrend({ label, records }: { label: string; records: MetricRecord[] }) {
  const latest = records[records.length - 1]
  return (
    <div className="border rounded p-3 bg-white">
      <div className="flex items-center justify-between gap-3 mb-2">
        <div className="font-mono text-xs text-gray-700 truncate" title={label}>{label}</div>
        <div className="text-xs text-gray-400">{records.length} points</div>
      </div>
      <TrendChart records={records} />
      {latest && (
        <div className="grid grid-cols-4 gap-2 mt-2 text-xs">
          <MetricPill color="bg-blue-500" label="CPU" value={latest.cpu_percent} />
          <MetricPill color="bg-emerald-500" label="Memory" value={latest.memory_percent} />
          <MetricPill color="bg-amber-500" label="Disk" value={latest.disk_percent} />
          <MetricPill color="bg-violet-500" label="NPU" value={latest.npu_percent} />
        </div>
      )}
    </div>
  )
}

function MetricPill({ color, label, value }: { color: string; label: string; value: number }) {
  return (
    <div className="flex items-center gap-1.5 text-gray-600">
      <span className={`w-2 h-2 rounded-full ${color}`} />
      <span>{label}</span>
      <span className="font-mono text-gray-800">{formatPercent(value)}</span>
    </div>
  )
}

function TrendChart({ records }: { records: MetricRecord[] }) {
  const width = 760
  const height = 150
  const padX = 26
  const padY = 14
  const metrics = [
    ['cpu_percent', '#2563eb'],
    ['memory_percent', '#059669'],
    ['disk_percent', '#d97706'],
    ['npu_percent', '#7c3aed'],
  ] as const

  return (
    <svg viewBox={`0 0 ${width} ${height}`} className="w-full h-[150px] bg-gray-50 rounded border">
      {[0, 25, 50, 75, 100].map(value => {
        const y = yFor(value, height, padY)
        return (
          <g key={value}>
            <line x1={padX} y1={y} x2={width - 8} y2={y} stroke="#e5e7eb" />
            <text x={4} y={y + 3} fontSize="10" fill="#94a3b8">{value}</text>
          </g>
        )
      })}
      {metrics.map(([key, color]) => (
        <polyline
          key={key}
          fill="none"
          stroke={color}
          strokeWidth="2"
          strokeLinecap="round"
          strokeLinejoin="round"
          points={linePoints(records, key, width, height, padX, padY)}
        />
      ))}
    </svg>
  )
}

function linePoints(records: MetricRecord[], key: keyof MetricRecord, width: number, height: number, padX: number, padY: number) {
  if (records.length === 0) return ''
  const usableWidth = width - padX - 10
  return records.map((record, index) => {
    const x = records.length === 1 ? padX : padX + (index / (records.length - 1)) * usableWidth
    const raw = Number(record[key] || 0)
    const y = yFor(raw, height, padY)
    return `${x.toFixed(1)},${y.toFixed(1)}`
  }).join(' ')
}

function yFor(value: number, height: number, padY: number) {
  const clamped = Math.max(0, Math.min(100, Number(value || 0)))
  return height - padY - (clamped / 100) * (height - padY * 2)
}

function formatPercent(value: number) {
  return `${Math.max(0, Math.min(100, Number(value || 0))).toFixed(1)}%`
}
