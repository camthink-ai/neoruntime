package secrets

import (
	"testing"
	"time"
)

func TestValidateTimestamp(t *testing.T) {
	now := time.Unix(1_719_876_543, 0)
	nowSec := now.Unix()
	cases := []struct {
		name string
		ts   int64
		want bool // want nil error
	}{
		{"zero skipped", 0, true},
		{"within past", nowSec - 299, true},
		{"within future", nowSec + 299, true},
		{"exact now", nowSec, true},
		{"expired past", nowSec - 301, false},
		{"expired future", nowSec + 301, false},
	}
	for _, c := range cases {
		err := ValidateTimestamp(c.ts, now)
		if got := err == nil; got != c.want {
			t.Errorf("%s: ValidateTimestamp(%d) err==nil=%v, want %v (err=%v)", c.name, c.ts, got, c.want, err)
		}
	}
}
