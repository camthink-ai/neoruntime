/**
 * Format manual datetime for the device time API.
 * Uses wall-clock components from the picker (local Date getters) and the
 * selected IANA timezone offset — not UTC (toISOString).
 */
export function formatManualDateTimeRFC3339(
  date: Date,
  timeZone: string
): string {
  const pad = (n: number) => String(n).padStart(2, '0');
  const wall = `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}T${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`;
  const offset = getTimezoneOffsetString(timeZone, date);
  return `${wall}${offset}`;
}

/**
 * Build a Date whose BROWSER-LOCAL wall clock reads the same numbers that
 * `instant` reads in `timeZone` — i.e. the device's current wall-clock time.
 *
 * The DateTimePicker is a wall-clock editor: it displays and edits a Date via
 * browser-local getters (date-fns `format`, getHours()…), and
 * `formatManualDateTimeRFC3339` then re-stamps those local numbers with the
 * device-TZ offset. To echo the device's current time through that pipeline
 * without display↔save drift (especially when the browser TZ ≠ device TZ), the
 * seed Date's LOCAL numbers must BE the device's current wall-clock numbers.
 * Hence this Intl round-trip: render the absolute device instant in the device
 * TZ, then rebuild a Date from those components as if they were local.
 */
export function toDeviceWallClockDate(instant: Date, timeZone: string): Date {
  const dtf = new Intl.DateTimeFormat('en-US', {
    timeZone,
    year: 'numeric',
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false,
  });
  const parts = new Map(dtf.formatToParts(instant).map(p => [p.type, p.value]));
  const year = Number(parts.get('year'));
  const month = Number(parts.get('month')) - 1;
  const day = Number(parts.get('day'));
  let hour = Number(parts.get('hour'));
  // Intl with hour12:false can emit "24" at midnight on some engines.
  if (hour === 24) hour = 0;
  const minute = Number(parts.get('minute'));
  const second = Number(parts.get('second'));
  return new Date(year, month, day, hour, minute, second);
}

function getTimezoneOffsetString(timeZone: string, date: Date): string {
  try {
    const token = new Intl.DateTimeFormat('en-US', {
      timeZone,
      timeZoneName: 'longOffset',
    })
      .formatToParts(date)
      .find(part => part.type === 'timeZoneName')?.value;

    if (!token) return '+00:00';

    const match = token.match(/(?:GMT|UTC)([+-])(\d{1,2})(?::?(\d{2}))?/i);
    if (!match) return '+00:00';

    const sign = match[1];
    const hours = match[2].padStart(2, '0');
    const minutes = (match[3] ?? '00').padStart(2, '0');
    return `${sign}${hours}:${minutes}`;
  } catch {
    return '+00:00';
  }
}
