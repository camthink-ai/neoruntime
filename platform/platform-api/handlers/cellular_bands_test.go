package handlers

import "testing"

func TestParseLTEBands(t *testing.T) {
	bands, err := parseLTEBands("1, 3:5;8")
	if err != nil {
		t.Fatal(err)
	}
	want := []int{1, 3, 5, 8}
	if len(bands) != len(want) {
		t.Fatalf("got %v want %v", bands, want)
	}
	for i := range want {
		if bands[i] != want[i] {
			t.Fatalf("got %v want %v", bands, want)
		}
	}
	if got := normalizeLTEBandsInput("3:1:3"); got != "3,1" {
		t.Fatalf("normalize got %q want 3,1", got)
	}
}

func TestLTEBandMaskHex(t *testing.T) {
	if got := lteBandMaskHex([]int{1, 3}); got != "5" {
		t.Fatalf("got %s", got)
	}
}

func TestParseQCFGBandTriple(t *testing.T) {
	raw := "AT+QCFG=\"band\"\r\n+QCFG: \"band\",0xbf0,0x42000001e00b0e38df,0x0\r\n\r\nOK\r\n"
	gsm, lte, tds, ok := parseQCFGBandTriple(raw)
	if !ok || gsm != "bf0" || lte != "42000001e00b0e38df" || tds != "0" {
		t.Fatalf("got ok=%v %s %s %s", ok, gsm, lte, tds)
	}
}

func TestParseMBNList(t *testing.T) {
	raw := `+QMBNCFG: "List",0,0,0,"ROW_Generic_3GPP",0x0,0` + "\n" +
		`+QMBNCFG: "List",11,1,1,"VoLTE_OPNMKT_CT",0x050113FC,202402281` + "\nOK\n"
	entries := parseMBNList(raw)
	if len(entries) != 2 {
		t.Fatalf("got %d entries", len(entries))
	}
	if entries[1].Name != "VoLTE_OPNMKT_CT" || !entries[1].Selected || !entries[1].Activated || entries[1].Index != 11 {
		t.Fatalf("got %+v", entries[1])
	}
}
