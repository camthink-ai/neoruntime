package handlers

import (
	"fmt"
	"math/big"
	"regexp"
	"strconv"
	"strings"
)

var qcfgBandRe = regexp.MustCompile(`(?i)\+QCFG:\s*"band"\s*,\s*(0x[0-9a-f]+)\s*,\s*(0x[0-9a-f]+)\s*,\s*(0x[0-9a-f]+)`)

func normalizeLTEBandsInput(s string) string {
	s = strings.TrimSpace(s)
	if s == "" {
		return ""
	}
	bands, err := parseLTEBands(s)
	if err != nil {
		return s
	}
	parts := make([]string, 0, len(bands))
	for _, b := range bands {
		parts = append(parts, strconv.Itoa(b))
	}
	return strings.Join(parts, ",")
}

func parseLTEBands(s string) ([]int, error) {
	s = strings.TrimSpace(s)
	if s == "" {
		return nil, nil
	}
	s = strings.ReplaceAll(s, ":", ",")
	s = strings.ReplaceAll(s, ";", ",")
	s = strings.ReplaceAll(s, " ", ",")
	seen := map[int]struct{}{}
	var out []int
	for _, part := range strings.Split(s, ",") {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		n, err := strconv.Atoi(part)
		if err != nil {
			return nil, fmt.Errorf("lte_bands: invalid band %q", part)
		}
		if n < 1 || n > 256 {
			return nil, fmt.Errorf("lte_bands: band %d out of range 1–256", n)
		}
		if _, ok := seen[n]; ok {
			continue
		}
		seen[n] = struct{}{}
		out = append(out, n)
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("lte_bands: empty after parse")
	}
	return out, nil
}

func lteBandMaskHex(bands []int) string {
	var mask big.Int
	for _, b := range bands {
		if b < 1 {
			continue
		}
		mask.SetBit(&mask, b-1, 1)
	}
	if mask.Sign() == 0 {
		return "0"
	}
	return strings.ToLower(mask.Text(16))
}

func hexNoPrefix(s string) string {
	s = strings.TrimSpace(strings.ToLower(s))
	s = strings.TrimPrefix(s, "0x")
	if s == "" {
		return "0"
	}
	return s
}

func parseQCFGBandTriple(raw string) (gsm, lte, tds string, ok bool) {
	m := qcfgBandRe.FindStringSubmatch(raw)
	if len(m) < 4 {
		return "", "", "", false
	}
	return hexNoPrefix(m[1]), hexNoPrefix(m[2]), hexNoPrefix(m[3]), true
}
