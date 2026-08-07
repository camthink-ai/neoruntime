import type { ListenerStats } from '../hooks/useDevices'

interface Props {
  devices: { online: boolean }[]
  listening: boolean
  iface: string
  stats: ListenerStats | null
}

export function StatusBar({ devices, listening, iface, stats }: Props) {
  const online = devices.filter(d => d.online).length
  const offline = devices.filter(d => !d.online).length

  return (
    <div className="flex items-center gap-4 px-4 py-1.5 bg-gray-50 border-t text-xs text-gray-500">
      <span className="flex items-center gap-1.5">
        <span className="inline-block w-2 h-2 rounded-full bg-green-500" />
        Online: {online}
      </span>
      <span className="flex items-center gap-1.5">
        <span className="inline-block w-2 h-2 rounded-full bg-gray-300" />
        Offline: {offline}
      </span>
      <span>Total: {devices.length}</span>
      <span className="flex-1" />
      {stats && (
        <span className={stats.recvCount > 0 ? 'text-green-600' : 'text-orange-500'}>
          Rx:{stats.recvCount} Events:{stats.eventCount}
        </span>
      )}
      <span>Interface: {iface || 'auto'}</span>
      <span className={listening ? 'text-green-600' : 'text-red-400'}>
        {listening ? 'Listening' : 'Stopped'}
      </span>
    </div>
  )
}
