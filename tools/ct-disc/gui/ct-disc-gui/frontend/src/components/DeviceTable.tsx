import type { Device } from '../hooks/useDevices'

interface Props {
  devices: Device[]
  selectedMac: string | null
  onSelect: (mac: string) => void
  selectedMacs: Set<string>
  onToggleSelect: (mac: string) => void
  onSelectAll: () => void
}

const COL_WIDTHS = {
  check: 'w-8',
  status: 'w-16',
  mac: '',
  sn: '',
  product: 'w-24',
  ip: 'w-36',
  fw: 'w-20',
}

export function DeviceTable({ devices, selectedMac, onSelect, selectedMacs, onToggleSelect, onSelectAll }: Props) {
  const online = devices.filter(d => d.online)
  const allSelected = online.length > 0 && online.every(d => d.mac && selectedMacs.has(d.mac))

  if (devices.length === 0) {
    return (
      <div className="flex flex-col items-center justify-center h-full text-gray-400 gap-3 px-4">
        <svg className="w-12 h-12 text-gray-300" fill="none" viewBox="0 0 24 24" stroke="currentColor">
          <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={1.5}
            d="M8.25 3v1.5M4.5 8.25H3m18 0h-1.5M4.5 12H3m18 0h-1.5m-15 3.75H3m18 0h-1.5M8.25 19.5V21M12 3v1.5m0 15V21m3.75-18v1.5m0 15V21m9-9h-1.5M4.5 12H3" />
        </svg>
        <p className="text-sm text-center">No devices found</p>
        <p className="text-xs text-center text-gray-300">Click "Scan" to discover devices on the network</p>
      </div>
    )
  }

  return (
    <div className="flex flex-col h-full">
      {/* Search */}
      <div className="px-3 py-2 border-b">
        <input
          type="text"
          placeholder="Filter devices..."
          className="w-full px-2 py-1.5 text-xs border rounded focus:outline-none focus:ring-1 focus:ring-blue-400 bg-gray-50"
          onChange={(e) => {
            const q = e.target.value.toLowerCase()
            const rows = document.querySelectorAll<HTMLTableRowElement>('.device-row')
            rows.forEach(row => {
              const text = row.textContent?.toLowerCase() || ''
              row.style.display = text.includes(q) ? '' : 'none'
            })
          }}
        />
      </div>

      {/* Table */}
      <div className="overflow-auto flex-1">
        <table className="w-full text-sm table-fixed">
          <thead className="sticky top-0 bg-gray-50 z-10">
            <tr className="border-b">
              <th className={`${COL_WIDTHS.check} px-2 py-1.5`}>
                <input
                  type="checkbox"
                  checked={allSelected}
                  onChange={onSelectAll}
                  className="w-3.5 h-3.5 rounded border-gray-300 text-blue-600 focus:ring-blue-500"
                />
              </th>
              <th className={`${COL_WIDTHS.status} text-left px-2 py-1.5 font-medium text-gray-500 text-xs`}>Status</th>
              <th className={`${COL_WIDTHS.ip} text-left px-2 py-1.5 font-medium text-gray-500 text-xs`}>IP</th>
              <th className={`${COL_WIDTHS.fw} text-left px-2 py-1.5 font-medium text-gray-500 text-xs`}>FW</th>
              <th className={`${COL_WIDTHS.product} text-left px-2 py-1.5 font-medium text-gray-500 text-xs`}>Product</th>
              <th className={`${COL_WIDTHS.mac} text-left px-2 py-1.5 font-medium text-gray-500 text-xs`}>MAC</th>
              <th className={`${COL_WIDTHS.sn} text-left px-2 py-1.5 font-medium text-gray-500 text-xs`}>SN</th>
            </tr>
          </thead>
          <tbody>
            {devices.map(d => {
              const mac = d.mac || d.sn
              const checked = d.mac ? selectedMacs.has(d.mac) : false
              const isRowSelected = selectedMac === d.mac || selectedMac === d.sn
              return (
                <tr
                  key={mac}
                  onClick={() => onSelect(mac)}
                  className={`device-row cursor-pointer border-b border-gray-50 ${isRowSelected ? 'bg-blue-50' : ''} ${checked ? 'row-selected' : ''}`}
                >
                  <td className={`${COL_WIDTHS.check} px-2 py-1.5`} onClick={e => e.stopPropagation()}>
                    <input
                      type="checkbox"
                      checked={checked}
                      disabled={!d.online}
                      onChange={() => d.mac && onToggleSelect(d.mac)}
                      className="w-3.5 h-3.5 rounded border-gray-300 text-blue-600 focus:ring-blue-500 disabled:opacity-30"
                    />
                  </td>
                  <td className={`${COL_WIDTHS.status} px-2 py-1.5`}>
                    <span className={`inline-block w-2 h-2 rounded-full ${d.online ? 'bg-green-500 status-pulse' : 'bg-gray-300'}`} />
                  </td>
                  <td className={`${COL_WIDTHS.ip} px-2 py-1.5 font-mono text-xs text-gray-600 truncate`} title={d.ip}>{d.ip}</td>
                  <td className={`${COL_WIDTHS.fw} px-2 py-1.5 text-xs text-gray-500 truncate`} title={d.fw}>{d.fw}</td>
                  <td className={`${COL_WIDTHS.product} px-2 py-1.5 text-xs truncate`} title={d.product}>{d.product}</td>
                  <td className={`${COL_WIDTHS.mac} px-2 py-1.5 font-mono text-xs text-gray-600 truncate`} title={d.mac || '-'}>{d.mac || '-'}</td>
                  <td className={`${COL_WIDTHS.sn} px-2 py-1.5 font-mono text-xs truncate`} title={d.sn}>{d.sn}</td>
                </tr>
              )
            })}
          </tbody>
        </table>
      </div>
    </div>
  )
}
