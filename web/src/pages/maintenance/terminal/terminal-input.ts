/** VT100 SS3 application keypad → ASCII (xterm numpad with Num Lock on) */
const SS3_KEYPAD_MAP: Record<string, string> = {
  '\x1bOp': '0',
  '\x1bOq': '1',
  '\x1bOr': '2',
  '\x1bOs': '3',
  '\x1bOt': '4',
  '\x1bOu': '5',
  '\x1bOv': '6',
  '\x1bOw': '7',
  '\x1bOx': '8',
  '\x1bOy': '9',
  '\x1bOl': '+',
  '\x1bOm': '-',
  '\x1bOn': '.',
  '\x1bOM': '\r',
};

/** CSI numpad (xterm rxvt mode) */
const CSI_KEYPAD_MAP: Record<string, string> = {
  '\x1b[11~': '1',
  '\x1b[12~': '2',
  '\x1b[13~': '3',
  '\x1b[14~': '4',
  '\x1b[15~': '5',
  '\x1b[17~': '6',
  '\x1b[18~': '7',
  '\x1b[19~': '8',
  '\x1b[20~': '9',
  '\x1b[21~': '0',
  '\x1b[3~': '\x7f', // Delete
};

const FULLWIDTH_DIGIT_OFFSET = '０'.charCodeAt(0) - '0'.charCodeAt(0);

export function normalizeTerminalInput(data: string): string {
  let out = data.replace(/[\uFF10-\uFF19]/g, ch => String.fromCharCode(ch.charCodeAt(0) - FULLWIDTH_DIGIT_OFFSET));

  for (const [seq, ch] of Object.entries(SS3_KEYPAD_MAP)) {
    if (out.includes(seq)) {
      out = out.split(seq).join(ch);
    }
  }
  for (const [seq, ch] of Object.entries(CSI_KEYPAD_MAP)) {
    if (out.includes(seq)) {
      out = out.split(seq).join(ch);
    }
  }
  return out;
}

/** Resolve ASCII digit from keydown (main row + numpad), with code fallback for IME. */
export function getDigitFromKeyboardEvent(event: KeyboardEvent): string | null {
  if (event.ctrlKey || event.altKey || event.metaKey) return null;

  if (/^[0-9]$/.test(event.key)) return event.key;

  const codeMatch = event.code.match(/^(?:Digit|Numpad)([0-9])$/);
  if (codeMatch) return codeMatch[1];

  return null;
}

export function isNumpadDecimalKey(event: KeyboardEvent): boolean {
  return (
    event.location === KeyboardEvent.DOM_KEY_LOCATION_NUMPAD
    && (event.key === '.' || event.code === 'NumpadDecimal')
  );
}

export function isNumpadEnterKey(event: KeyboardEvent): boolean {
  return (
    event.location === KeyboardEvent.DOM_KEY_LOCATION_NUMPAD
    && event.key === 'Enter'
  );
}

/** Disable IME / autocorrect on xterm's hidden textarea so digits reach the terminal. */
export function configureXtermTextarea(container: HTMLElement) {
  const textarea = container.querySelector('.xterm-helper-textarea');
  if (!(textarea instanceof HTMLTextAreaElement)) return;

  textarea.inputMode = 'none';
  textarea.autocomplete = 'off';
  textarea.setAttribute('autocorrect', 'off');
  textarea.setAttribute('autocapitalize', 'off');
  textarea.spellcheck = false;
  textarea.lang = 'en';
  textarea.setAttribute('data-gramm', 'false');
  textarea.setAttribute('data-gramm_editor', 'false');
  textarea.setAttribute('data-enable-grammarly', 'false');
}
