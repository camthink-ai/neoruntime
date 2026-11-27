/**
 * The Go cellular/network handlers return English status and error messages.
 * Map the known finite set onto i18n keys so the UI renders them in the
 * active locale; unknown strings pass through untouched.
 */

type Tf = (key: string, fallback: string) => string;

/** Exact-match backend messages → i18n keys. */
export const EXACT_MESSAGES: Record<string, string> = {
  // /api/v1/cellular/status probe states (cellular_probe.go)
  'Cellular hardware profile is not configured.':
    'sys.cellular.msg_hw_profile_missing',
  'No cellular hardware detected on this device.': 'sys.cellular.msg_hw_absent',
  'Detecting cellular hardware…': 'sys.cellular.msg_detecting',
  'Module power is off.': 'sys.cellular.msg_power_off',
  'Power enabled but module not on USB. Check drivers / GPIO / insertion.':
    'sys.cellular.msg_power_no_module',
  'Module re-enumerating on USB…': 'sys.cellular.msg_reenumerating',
  'Module on USB but no AT/QMI/MBIM control device.':
    'sys.cellular.msg_no_control_dev',
  'Cellular data is off.': 'sys.cellular.msg_data_off',
  'Module detected. Dialer binary from hardware profile is not installed yet.':
    'sys.cellular.msg_dialer_missing',
  'Dialer running; waiting for data session.': 'sys.cellular.msg_dialing',
  'Module ready; auto-dial will start shortly.': 'sys.cellular.msg_auto_dial_soon',
  'Cellular data session is up.': 'sys.cellular.msg_session_up',
  'No SIM detected. Insert a SIM to dial.': 'sys.cellular.msg_no_sim',
  'SIM not ready; modem is re-reading the card…': 'sys.cellular.msg_sim_rereading',
  // /api/v1/cellular/config + dial responses (cellular.go)
  'Enable module power before connecting.': 'sys.cellular.msg_need_power',
  'Enable cellular data before connecting.': 'sys.cellular.msg_need_data',
  'Cellular hardware is not available on this device.':
    'sys.cellular.msg_hw_absent',
  'Cellular dialer is not available on this firmware yet.':
    'sys.cellular.msg_dialer_unavailable',
  'Dialer started.': 'sys.cellular.msg_dial_started',
  'No cellular module detected. Cellular has been turned off.':
    'sys.cellular.enable_no_module',
  'Config saved, but GPIO power failed — line busy. Stop ct-cell (or kill foreign gpioset) and retry.':
    'sys.cellular.msg_gpio_busy',
  // GNSS status messages (cellular_gnss.go)
  'GNSS is off.': 'sys.cellular.msg_gnss_off',
  'GNSS fix acquired.': 'sys.cellular.msg_gnss_fix_acquired',
  'AT port busy; retry shortly.': 'sys.cellular.msg_at_busy',
  'No AT port.': 'sys.cellular.msg_no_at_port',
  'Unexpected AT+QGPS? reply.': 'sys.cellular.msg_gnss_unexpected_reply',
  'GNSS enabled in config; engine not started yet.':
    'sys.cellular.msg_gnss_engine_pending',
  'GNSS active; location query failed.': 'sys.cellular.msg_gnss_loc_fail',
  'Waiting for GNSS fix…': 'sys.cellular.msg_gnss_waiting',
  'GNSS active; no fix yet.': 'sys.cellular.msg_gnss_no_fix',
  // Debug / qlog / MBN responses (cellular_debug.go, cellular_qlog.go, cellular_verizon.go)
  'Cellular hardware is not available.': 'sys.cellular.msg_hw_unavailable',
  'No AT port available (module busy or offline).':
    'sys.cellular.msg_at_unavailable_busy',
  'No AT port available.': 'sys.cellular.msg_at_unavailable',
  'Invalid host.': 'sys.cellular.msg_invalid_host',
  'No DM port found (/dev/ttyUSB*).': 'sys.cellular.msg_no_dm_port',
  'Invalid DM port.': 'sys.cellular.msg_invalid_dm_port',
  'QLog already running': 'sys.cellular.msg_qlog_already_running',
  'Stop QLog before clearing logs.': 'sys.cellular.msg_qlog_stop_first',
  // Ping / AT debug validation (cellular_debug.go)
  'ping failed': 'sys.cellular.msg_ping_failed',
  'Cellular interface profile is not configured':
    'sys.cellular.msg_ping_iface_no_profile',
  'Invalid iface': 'sys.cellular.msg_invalid_iface',
  'Iface is not a cellular interface': 'sys.cellular.msg_not_cellular_iface',
  'Cellular data session is not up': 'sys.cellular.msg_no_session',
  'command is empty': 'sys.cellular.msg_at_empty',
  'command too long': 'sys.cellular.msg_at_too_long',
  'only a single AT command is allowed': 'sys.cellular.msg_at_single',
  'command must start with AT': 'sys.cellular.msg_at_start_at',
  // Config validation errors (cellular.go, surfaced after the save prefix)
  'auth must be none, pap, or chap': 'sys.cellular.msg_auth_invalid',
  'ip_type must be ipv4, ipv6, or ipv4v6': 'sys.cellular.msg_ip_type_invalid',
  'carrier_profile must be generic or verizon':
    'sys.cellular.msg_carrier_invalid',
  'usbnet must be qmi, ecm, or mbim': 'sys.cellular.msg_usbnet_invalid',
  'network_mode must be auto, lte, or wcdma':
    'sys.cellular.msg_network_mode_invalid',
  'pdp_cid must be between 1 and 16': 'sys.cellular.msg_pdp_cid_range',
  'pin must be at most 8 digits': 'sys.cellular.msg_pin_too_long',
  'pin must be numeric': 'sys.cellular.msg_pin_numeric',
  'plmn must be 5–6 digits (MCC+MNC)': 'sys.cellular.msg_plmn_digits',
  'plmn must be numeric': 'sys.cellular.msg_plmn_numeric',
  'mtu must be 0 (default) or 576–1500': 'sys.cellular.msg_mtu_range',
  'mbn name too long': 'sys.cellular.msg_mbn_too_long',
  'Module not detected on USB. Check insertion, drivers, and GPIO power.':
    'sys.cellular.msg_module_not_detected',
  // Network config validation (network.go)
  "mode must be 'dhcp' or 'static'": 'sys.network.msg_mode_invalid',
  'ip_address is required for static mode': 'sys.network.msg_ip_required',
  'subnet_mask is required for static mode': 'sys.network.msg_mask_required',
  'invalid ip_address format': 'sys.network.msg_ip_invalid',
  'invalid subnet_mask format': 'sys.network.msg_mask_invalid',
  'invalid gateway format': 'sys.network.msg_gateway_invalid',
  'invalid dns1 format': 'sys.network.msg_dns1_invalid',
  'invalid dns2 format': 'sys.network.msg_dns2_invalid',
  'Failed to get network configuration': 'sys.network.msg_get_config_failed',
  'Failed to get network interfaces': 'sys.network.msg_get_ifaces_failed',
};

/** Messages with a dynamic suffix; the suffix is technical detail and passes through. */
export const PREFIX_MESSAGES: Array<[prefix: string, key: string]> = [
  ['Failed to power module: ', 'sys.cellular.msg_power_fail'],
  ['Dialer start failed: ', 'sys.cellular.msg_dial_fail'],
  ['failed to save cellular config: ', 'sys.cellular.msg_save_fail'],
  ['Config saved, but applying USB network mode failed: ', 'sys.cellular.msg_usbnet_apply_fail'],
  ['Config saved, but applying network mode/roaming failed: ', 'sys.cellular.msg_netmode_apply_fail'],
  ['Config saved, but applying GNSS failed: ', 'sys.cellular.msg_gnss_apply_fail'],
  ['Failed to query GNSS state: ', 'sys.cellular.msg_gnss_query_fail'],
  ['DM port not present: ', 'sys.cellular.msg_dm_port_missing'],
  ['Cannot create log dir: ', 'sys.cellular.msg_logdir_fail'],
  ['Failed to start qlog: ', 'sys.cellular.msg_qlog_start_fail'],
  ['Cellular data session is not up on ', 'sys.cellular.msg_no_session_on'],
  ['Failed to encode network config: ', 'sys.network.msg_encode_fail'],
  ['Failed to apply network config: ', 'sys.network.msg_apply_fail'],
];

const REG_STATES = [
  'unknown',
  'not_registered',
  'searching',
  'registered',
  'roaming',
  'denied',
] as const;

/**
 * Translate a backend status/error message when it is one of the known
 * strings; otherwise return the raw (trimmed) message.
 */
export function localizeBackendMessage(
  raw: string | null | undefined,
  t: Tf
): string {
  const msg = raw?.trim();
  if (!msg) return '';

  const exact = EXACT_MESSAGES[msg];
  if (exact) return t(exact, msg);

  for (const [prefix, key] of PREFIX_MESSAGES) {
    if (msg.startsWith(prefix)) {
      // A known validation error after the prefix (e.g. "failed to save
      // cellular config: plmn must be 5–6 digits (MCC+MNC)") localizes as a
      // whole; anything else is technical detail and passes through.
      const suffix = msg.slice(prefix.length);
      const suffixKey = EXACT_MESSAGES[suffix];
      const suffixText = suffixKey ? t(suffixKey, suffix) : suffix;
      return `${t(key, prefix)}${suffixText}`;
    }
  }
  return msg;
}

/** Localized registration-state label for the network metric chip. */
export function regStateLabel(
  state: string | null | undefined,
  t: Tf
): string {
  const s = state?.trim();
  if (!s) return '';
  return (REG_STATES as readonly string[]).includes(s)
    ? t(`sys.cellular.reg_state_${s}`, s.replace(/_/g, ' '))
    : s;
}
