type LogLevel = 'debug' | 'info' | 'warn' | 'silent';

const LEVEL_ORDER: Record<LogLevel, number> = {
  debug: 10,
  info: 20,
  warn: 30,
  silent: 99,
};

let currentLevel: LogLevel = 'warn';

export function setLogLevel(level: LogLevel): void {
  currentLevel = level;
}

export interface Logger {
  debug(msg: string, ...args: unknown[]): void;
  info(msg: string, ...args: unknown[]): void;
  warn(msg: string, ...args: unknown[]): void;
  error(msg: string, ...args: unknown[]): void;
}

export function createLogger(tag: string): Logger {
  const prefix = `[player:${tag}]`;
  return {
    debug(msg, ...args) {
      if (LEVEL_ORDER[currentLevel] <= 10) console.log(prefix, msg, ...args);
    },
    info(msg, ...args) {
      if (LEVEL_ORDER[currentLevel] <= 20) console.log(prefix, msg, ...args);
    },
    warn(msg, ...args) {
      if (LEVEL_ORDER[currentLevel] <= 30) console.warn(prefix, msg, ...args);
    },
    error(msg, ...args) {
      if (LEVEL_ORDER[currentLevel] < 99) console.error(prefix, msg, ...args);
    },
  };
}
