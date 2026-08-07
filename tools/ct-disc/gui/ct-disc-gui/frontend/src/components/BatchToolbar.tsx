import type { Device } from '../hooks/useDevices'

interface Props {
  selectedDevices: Device[]
  onClearSelection: () => void
  onBatchCommand: () => void
  onBatchNetwork: () => void
}

export function BatchToolbar({ selectedDevices, onClearSelection, onBatchCommand, onBatchNetwork }: Props) {
  if (selectedDevices.length === 0) return null

  return (
    <div className="flex items-center gap-3 px-4 py-2 bg-blue-600 text-white animate-slide-up">
      <span className="text-sm font-medium">
        {selectedDevices.length} device{selectedDevices.length !== 1 ? 's' : ''} selected
      </span>
      <div className="flex-1" />
      <button
        onClick={onBatchCommand}
        className="px-3 py-1 text-xs bg-white/15 hover:bg-white/25 rounded transition-colors"
      >
        Send Command
      </button>
      <button
        onClick={onBatchNetwork}
        className="px-3 py-1 text-xs bg-white/15 hover:bg-white/25 rounded transition-colors"
      >
        Network Config
      </button>
      <button
        onClick={onClearSelection}
        className="px-3 py-1 text-xs bg-white/10 hover:bg-white/20 rounded transition-colors"
      >
        Clear Selection
      </button>
    </div>
  )
}
