package secrets

import (
	"errors"
	"time"
)

// FreshnessWindow is the maximum clock skew tolerated between client and
// device for a timestamped login / change-password request. ±5 minutes is wide
// enough to absorb NTP convergence on a freshly booted edge device, yet narrow
// enough to make replaying a captured ciphertext within the window uneconomical.
const FreshnessWindow = 5 * time.Minute

// ErrTimestampOutOfWindow is returned by ValidateTimestamp when the supplied
// unix-second timestamp falls outside [now-FreshnessWindow, now+FreshnessWindow].
var ErrTimestampOutOfWindow = errors.New("secrets: timestamp outside allowed window")

// ValidateTimestamp returns nil if ts (unix seconds) is within FreshnessWindow
// of now in either direction. A zero ts is treated as "absent" and accepted
// (the caller decides whether to enforce presence); this preserves login for
// older frontends that do not send a timestamp during a rollout.
func ValidateTimestamp(ts int64, now time.Time) error {
	if ts == 0 {
		return nil
	}
	delta := time.Unix(ts, 0).Sub(now)
	if delta < 0 {
		delta = -delta
	}
	if delta > FreshnessWindow {
		return ErrTimestampOutOfWindow
	}
	return nil
}
