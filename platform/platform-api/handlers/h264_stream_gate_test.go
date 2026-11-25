package handlers

import (
	"encoding/binary"
	"testing"
	"time"
)

func avcc(nals ...[]byte) []byte {
	out := []byte{}
	for _, n := range nals {
		l := make([]byte, 4)
		binary.BigEndian.PutUint32(l, uint32(len(n)))
		out = append(out, l...)
		out = append(out, n...)
	}
	return out
}

func nalTypeOf(entry []byte) byte { return entry[4] & 0x1f }

func TestExtractParameterSets(t *testing.T) {
	sps := []byte{0x67, 0x64, 0x00, 0x1f}
	pps := []byte{0x68, 0xeb, 0xec, 0xb2}
	sei := []byte{0x06, 0x05, 0x01, 0xff}
	idr := []byte{0x65, 0x88, 0x84, 0x00}

	t.Run("IDR access unit keeps only SPS and PPS", func(t *testing.T) {
		got := extractParameterSets(avcc(sps, pps, sei, idr))
		if len(got) != 2 {
			t.Fatalf("want 2 entries, got %d", len(got))
		}
		if nalTypeOf(got[0]) != 7 || nalTypeOf(got[1]) != 8 {
			t.Fatalf("want SPS then PPS, got types %d,%d", nalTypeOf(got[0]), nalTypeOf(got[1]))
		}
		for i, e := range got {
			if len(e) != 4+len(sps) && len(e) != 4+len(pps) {
				t.Fatalf("entry %d not length-prefixed: %d bytes", i, len(e))
			}
		}
	})

	t.Run("delta frame yields nothing", func(t *testing.T) {
		if got := extractParameterSets(avcc(sei, []byte{0x41, 0x9a, 0x02})); len(got) != 0 {
			t.Fatalf("want 0 entries from a P frame, got %d", len(got))
		}
	})

	t.Run("truncated length prefix stops the walk", func(t *testing.T) {
		bad := append(avcc(sps), 0x00, 0x00, 0x00, 0xff, 0x67)
		if got := extractParameterSets(bad); len(got) != 1 {
			t.Fatalf("want 1 entry before truncation, got %d", len(got))
		}
	})
}

func TestMergeParameterSets(t *testing.T) {
	sps := avcc([]byte{0x67, 0x01})
	pps := avcc([]byte{0x68, 0x01})
	sps2 := avcc([]byte{0x67, 0x02})

	t.Run("SPS replaces the generation", func(t *testing.T) {
		s := &H264Stream{}
		s.mergeParameterSets(7, [][]byte{sps, pps})
		s.mergeParameterSets(7, [][]byte{sps2})
		if len(s.spsPPS) != 1 || nalTypeOf(s.spsPPS[0]) != 7 {
			t.Fatalf("SPS must replace, got %d entries", len(s.spsPPS))
		}
	})

	t.Run("PPS without SPS is dropped", func(t *testing.T) {
		s := &H264Stream{}
		s.mergeParameterSets(8, [][]byte{pps})
		if len(s.spsPPS) != 0 {
			t.Fatalf("PPS must not start a set, got %d entries", len(s.spsPPS))
		}
	})

	t.Run("PPS after SPS appends, capped at 2", func(t *testing.T) {
		s := &H264Stream{}
		s.mergeParameterSets(7, [][]byte{sps})
		s.mergeParameterSets(8, [][]byte{pps, pps, pps})
		if len(s.spsPPS) != 2 {
			t.Fatalf("cache must cap at 2, got %d", len(s.spsPPS))
		}
	})
}

func TestClientGatePasses(t *testing.T) {
	key := []byte{0xff, 0xd8, 0xff, 0x00, 0x00, 0x01} // FrameType=0
	delta := []byte{0xff, 0xd8, 0xff, 0x00, 0x01, 0x02} // FrameType=1

	t.Run("synced client passes everything", func(t *testing.T) {
		if pass, synced := clientGatePasses(delta, true, 0); !pass || !synced {
			t.Fatalf("synced client: want pass+synced, got %v,%v", pass, synced)
		}
	})

	t.Run("keyframe syncs a fresh client", func(t *testing.T) {
		if pass, synced := clientGatePasses(key, false, 0); !pass || !synced {
			t.Fatalf("keyframe: want pass+synced, got %v,%v", pass, synced)
		}
	})

	t.Run("delta is held before the timeout", func(t *testing.T) {
		if pass, synced := clientGatePasses(delta, false, clientSyncGateTimeout-time.Millisecond); pass || synced {
			t.Fatalf("early delta: want hold, got pass=%v synced=%v", pass, synced)
		}
	})

	t.Run("gate degrades to pass-through after the timeout", func(t *testing.T) {
		if pass, synced := clientGatePasses(delta, false, clientSyncGateTimeout); !pass || synced {
			t.Fatalf("late delta: want pass-through without sync, got %v,%v", pass, synced)
		}
	})
}
