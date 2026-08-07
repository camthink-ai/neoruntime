package utils

// MatchTopic performs wildcard matching for topic patterns
// Supports:
//   - Exact match: "app/myapp/events" matches "app/myapp/events"
//   - Suffix wildcard: "app/myapp/*" matches "app/myapp/events", "app/myapp/alerts"
//   - Single segment wildcard: "app/*/events" matches "app/myapp/events", "app/yourapp/events"
//   - Multi-level wildcard: "app/**" matches "app/myapp/events", "app/myapp/sub/events"
//   - Multi-level with suffix: "app/**/events" matches "app/a/b/events"
func MatchTopic(topic, pattern string) bool {
	// Exact match
	if pattern == topic {
		return true
	}

	// Handle "**" (multi-level wildcard) - matches everything
	if pattern == "**" {
		return true
	}

	// Handle single "*" as match-all (same as **)
	if pattern == "*" {
		return true
	}

	// Split into parts for matching
	topicParts := splitTopic(topic)
	patternParts := splitTopic(pattern)

	return matchParts(topicParts, patternParts)
}

// matchParts recursively matches topic segments against pattern segments
func matchParts(topicParts, patternParts []string) bool {
	ti, pi := 0, 0

	for pi < len(patternParts) {
		if patternParts[pi] == "**" {
			// ** can match zero or more segments
			// Try matching remaining pattern against each possible position
			remainingPattern := patternParts[pi+1:]
			if len(remainingPattern) == 0 {
				// ** at end matches everything
				return true
			}
			// Try consuming 0, 1, 2, ... topic segments
			for k := ti; k <= len(topicParts); k++ {
				if matchParts(topicParts[k:], remainingPattern) {
					return true
				}
			}
			return false
		}

		if ti >= len(topicParts) {
			return false
		}

		if patternParts[pi] == "*" {
			// Single segment wildcard - match one segment
			ti++
			pi++
		} else if patternParts[pi] == topicParts[ti] {
			// Exact match
			ti++
			pi++
		} else {
			return false
		}
	}

	// All parts matched
	return ti == len(topicParts) && pi == len(patternParts)
}

// findDoubleWildcard finds "**" in pattern, returns index or -1
func findDoubleWildcard(pattern string) int {
	for i := 0; i < len(pattern)-1; i++ {
		if pattern[i] == '*' && pattern[i+1] == '*' {
			return i
		}
	}
	return -1
}

// splitTopic splits topic by "/"
func splitTopic(topic string) []string {
	if topic == "" {
		return []string{}
	}

	parts := []string{}
	start := 0

	for i := 0; i < len(topic); i++ {
		if topic[i] == '/' {
			if i > start {
				parts = append(parts, topic[start:i])
			}
			start = i + 1
		}
	}

	if start < len(topic) {
		parts = append(parts, topic[start:])
	}

	return parts
}
