package utils

import (
	"testing"
)

func TestMatchTopic_ExactMatch(t *testing.T) {
	cases := []struct {
		topic, pattern string
		want           bool
	}{
		{"app/test/alert", "app/test/alert", true},
		{"model/person_v1/detection", "model/person_v1/detection", true},
		{"system/health", "system/health", true},
		{"app/test/alert", "app/test/other", false},
		{"app/test", "app/test/alert", false},
		{"", "", true},
	}

	for _, tc := range cases {
		got := MatchTopic(tc.topic, tc.pattern)
		if got != tc.want {
			t.Errorf("MatchTopic(%q, %q) = %v, want %v", tc.topic, tc.pattern, got, tc.want)
		}
	}
}

func TestMatchTopic_SingleWildcard(t *testing.T) {
	cases := []struct {
		topic, pattern string
		want           bool
	}{
		// Suffix wildcard
		{"app/myapp/events", "app/myapp/*", true},
		{"app/myapp/alerts", "app/myapp/*", true},

		// Middle wildcard
		{"app/myapp/events", "app/*/events", true},
		{"app/yourapp/events", "app/*/events", true},
		{"app/any/events", "app/*/events", true},

		// Leading wildcard
		{"foo/bar/events", "*/bar/events", true},

		// * should NOT match multiple segments
		{"app/foo/bar/events", "app/*/events", false},

		// * should NOT match empty segment
		{"app/events", "app/*/events", false},

		// Multiple wildcards
		{"a/b/c", "*/*/*", true},
		{"a/b/c", "*/*", false},
	}

	for _, tc := range cases {
		got := MatchTopic(tc.topic, tc.pattern)
		if got != tc.want {
			t.Errorf("MatchTopic(%q, %q) = %v, want %v", tc.topic, tc.pattern, got, tc.want)
		}
	}
}

func TestMatchTopic_DoubleWildcard(t *testing.T) {
	cases := []struct {
		topic, pattern string
		want           bool
	}{
		// Global match
		{"anything", "**", true},
		{"a/b/c/d", "**", true},

		// Suffix multi-level
		{"app/foo", "app/**", true},
		{"app/foo/bar", "app/**", true},
		{"app/foo/bar/baz", "app/**", true},

		// ** should not match across different prefix
		{"system/foo", "app/**", false},

		// ** with suffix
		{"app/foo/bar/events", "app/**/events", true},
		{"app/events", "app/**/events", true},
		{"app/a/b/c/events", "app/**/events", true},
		{"app/foo/bar/other", "app/**/events", false},

		// Prefix match only
		{"app", "app/**", true},
	}

	for _, tc := range cases {
		got := MatchTopic(tc.topic, tc.pattern)
		if got != tc.want {
			t.Errorf("MatchTopic(%q, %q) = %v, want %v", tc.topic, tc.pattern, got, tc.want)
		}
	}
}

func TestSplitTopic(t *testing.T) {
	cases := []struct {
		input string
		want  []string
	}{
		{"", nil},
		{"app", []string{"app"}},
		{"app/test", []string{"app", "test"}},
		{"app/test/alert", []string{"app", "test", "alert"}},
		{"a/b/c/d/e", []string{"a", "b", "c", "d", "e"}},
	}

	for _, tc := range cases {
		got := splitTopic(tc.input)
		if len(got) == 0 && len(tc.want) == 0 {
			continue
		}
		if len(got) != len(tc.want) {
			t.Errorf("splitTopic(%q) = %v, want %v", tc.input, got, tc.want)
			continue
		}
		for i := range got {
			if got[i] != tc.want[i] {
				t.Errorf("splitTopic(%q)[%d] = %q, want %q", tc.input, i, got[i], tc.want[i])
			}
		}
	}
}

func TestFindDoubleWildcard(t *testing.T) {
	cases := []struct {
		input string
		want  int
	}{
		{"**", 0},
		{"app/**", 4},
		{"app/**/events", 4},
		{"*", -1},
		{"app/test", -1},
		{"", -1},
	}

	for _, tc := range cases {
		got := findDoubleWildcard(tc.input)
		if got != tc.want {
			t.Errorf("findDoubleWildcard(%q) = %d, want %d", tc.input, got, tc.want)
		}
	}
}

func BenchmarkMatchTopic_Exact(b *testing.B) {
	for i := 0; i < b.N; i++ {
		MatchTopic("model/person_v1/detections", "model/person_v1/detections")
	}
}

func BenchmarkMatchTopic_SingleWildcard(b *testing.B) {
	for i := 0; i < b.N; i++ {
		MatchTopic("model/person_v1/detections", "model/*/detections")
	}
}

func BenchmarkMatchTopic_DoubleWildcard(b *testing.B) {
	for i := 0; i < b.N; i++ {
		MatchTopic("app/foo/bar/baz/events", "app/**")
	}
}

func BenchmarkMatchTopic_DoubleWildcardWithSuffix(b *testing.B) {
	for i := 0; i < b.N; i++ {
		MatchTopic("app/foo/bar/baz/events", "app/**/events")
	}
}
