import type { MockMethod } from 'vite-plugin-mock';
import type { LogFile, LogLine } from '../types/log';

const mockFiles: LogFile[] = [
  {
    id: '1',
    name: 'event-bus.log',
    path: '/var/log/event-bus.log',
    modified_time: 'Modified 2m ago',
    size: 1258291,
    service: 'all',
  },
  {
    id: '2',
    name: 'platform-api.log',
    path: '/var/log/platform-api.log',
    modified_time: 'Modified 15m ago',
    size: 467968,
    service: 'platform',
  },
  {
    id: '3',
    name: 'camera-daemon.crash.log',
    path: '/var/log/camera-daemon.crash.log',
    modified_time: 'Modified 1h ago',
    size: 91136,
    service: 'camera',
  },
  {
    id: '4',
    name: 'access-control.log',
    path: '/var/log/access-control.log',
    modified_time: 'Modified 3h ago',
    size: 2516582,
    service: 'security',
  },
  {
    id: '5',
    name: 'security-audit.log',
    path: '/var/log/security-audit.log',
    modified_time: 'Modified 5h ago',
    size: 12288,
    service: 'security',
  },
];

const generateMockLines = (
  fileId: string,
  linesCount: number,
  filterText?: string
): LogLine[] => {
  const lines: LogLine[] = [];
  const baseTime = new Date('2023-10-27T14:22:01');

  for (let i = 0; i < linesCount; i++) {
    const time = new Date(baseTime.getTime() + i * 1000 * 20); // increment by 20s
    let level = 'INFO';
    let message = `Initializing connection to internal message broker... (line ${i})`;

    // Some basic variation to mimic the screenshot
    if (i % 5 === 1) {
      level = 'SUCCESS';
      message = `Handshake completed. Protocol: AMQP 0-9-1`;
    } else if (i % 5 === 2) {
      level = 'INFO';
      message = `Subscribing to topic: /telemetry/sensors/gate_04`;
    } else if (i % 5 === 3) {
      level = 'ERROR';
      message = `Connection timeout: peer disconnected abruptly at 10.0.4.12`;
    } else if (i % 5 === 4) {
      level = 'WARN';
      message = `Retrying connection in 5000ms... (Attempt 1/5)`;
    }

    // specific variation for different files
    if (fileId === '3') {
      level = 'ERROR';
      message = `CRITICAL: Daemon crashed with SIGSEGV at 0x0000000000000000`;
    } else if (fileId === '2') {
      level = i % 2 === 0 ? 'INFO' : 'SUCCESS';
      message = `API Request POST /api/v1/auth - 200 OK - ${Math.floor(Math.random() * 100)}ms`;
    }

    if (
      !filterText
      || message.toLowerCase().includes(filterText.toLowerCase())
    ) {
      lines.push({
        timestamp: `[${time.getFullYear()}-${String(time.getMonth() + 1).padStart(2, '0')}-${String(time.getDate()).padStart(2, '0')} ${String(time.getHours()).padStart(2, '0')}:${String(time.getMinutes()).padStart(2, '0')}:${String(time.getSeconds()).padStart(2, '0')}]`,
        level,
        message,
        raw: `${level}: ${message}`,
      });
    }
  }

  return lines;
};

export default [
  {
    url: '/api/logs/files',
    method: 'get',
    response: ({ query }: { query: { service?: string } }) => {
      const { service } = query;
      let data = mockFiles;
      if (service && service !== 'all') {
        data = mockFiles.filter(f => f.service === service);
      }
      return {
        code: 200,
        data,
        message: 'success',
      };
    },
  },
  {
    url: '/api/logs/files/:id/content',
    method: 'get',
    response: ({ query: { filterText, lines = '100' } }: any, req: any) => {
      // The path variable `id` is not cleanly parsed in query. In vite-plugin-mock, we might need a regex or get it from `req.url`.
      // Let's extract file ID from URL logic:
      const match = req.url.match(/\/api\/logs\/files\/([^/]+)\/content/);
      const id = match ? match[1] : '1';
      const linesCount = parseInt(lines as string, 10) || 100;

      return {
        code: 200,
        data: generateMockLines(id, linesCount, filterText),
        message: 'success',
      };
    },
  },
] as MockMethod[];
