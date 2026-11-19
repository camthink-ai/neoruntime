package handlers

import "testing"

func TestParseCGDCONTContextsVerizon(t *testing.T) {
	raw := "" +
		"AT+CGDCONT?\n" +
		`+CGDCONT: 1,"IPV4V6","vzwims","0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0",0,0,0,0` + "\n" +
		`+CGDCONT: 2,"IPV4V6","vzwadmin","0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0",0,0,0,0` + "\n" +
		`+CGDCONT: 3,"IPV4V6","vzwinternet","0.0.0.0.0.0.0.0.0.0.0.0.0.0.0.0",0,0,0,0` + "\n" +
		"OK\n"
	got := parseCGDCONTContexts(raw)
	if len(got) != 3 {
		t.Fatalf("len=%d want 3: %+v", len(got), got)
	}
	if !verizonDataContextOK(got) {
		t.Fatal("expected CID3 vzwinternet OK")
	}
	if verizonIMSLooksBroken(got) {
		t.Fatal("vzwims on CID1 should not look broken")
	}
}

func TestVerizonIMSLooksBroken(t *testing.T) {
	raw := `+CGDCONT: 1,"IPV4V6","vzwinternet","0.0.0.0",0,0,0,0` + "\n" +
		`+CGDCONT: 3,"IPV4V6","vzwinternet","0.0.0.0",0,0,0,0` + "\n"
	got := parseCGDCONTContexts(raw)
	if !verizonIMSLooksBroken(got) {
		t.Fatal("CID1=vzwinternet should look broken")
	}
	if !verizonDataContextOK(got) {
		t.Fatal("CID3 still OK")
	}
}

func TestNormalizeVerizonConfig(t *testing.T) {
	cfg := CellularConfig{
		CarrierProfile: "verizon",
		Usbnet:         "mbim",
		PDPCID:         1,
		APN:            "",
		IPType:         "ipv4",
		Auth:           "",
	}
	normalizeVerizonConfig(&cfg)
	if cfg.PDPCID != 3 || cfg.APN != "vzwinternet" || cfg.IPType != "ipv4v6" || cfg.Usbnet != "qmi" || cfg.Auth != "none" {
		t.Fatalf("got %+v", cfg)
	}

	generic := CellularConfig{CarrierProfile: "generic", Usbnet: "mbim", PDPCID: 1, APN: "cmnet", IPType: "ipv4"}
	normalizeVerizonConfig(&generic)
	if generic.Usbnet != "mbim" || generic.PDPCID != 1 {
		t.Fatalf("generic should be unchanged: %+v", generic)
	}
}

func TestNormalizeVerizonKeepsExplicitAPN(t *testing.T) {
	cfg := CellularConfig{CarrierProfile: "verizon", Usbnet: "qmi", PDPCID: 1, APN: "custom.vzw", IPType: "ipv4v6"}
	normalizeVerizonConfig(&cfg)
	if cfg.APN != "custom.vzw" || cfg.PDPCID != 3 {
		t.Fatalf("got %+v", cfg)
	}
}

func TestShouldUseVerizonDial(t *testing.T) {
	cases := []struct {
		profile, imsi, op string
		mbn               bool
		want              bool
	}{
		{"verizon", "", "", false, true},
		{"generic", "311480123456789", "", false, true},
		{"", "", "Verizon", false, true},
		{"generic", "", "", true, true},
		{"generic", "460026592329973", "CHINA MOBILE", false, false},
		{"generic", "46001", "CHN-CT", false, false},
	}
	for _, tc := range cases {
		if got := shouldUseVerizonDial(tc.profile, tc.imsi, tc.op, tc.mbn); got != tc.want {
			t.Fatalf("profile=%q imsi=%q op=%q mbn=%v: got %v want %v",
				tc.profile, tc.imsi, tc.op, tc.mbn, got, tc.want)
		}
	}
}

func TestVerizonMBNActiveFromAT(t *testing.T) {
	raw := "" +
		`+QMBNCFG: "List",0,0,0,"ROW_Generic_3GPP",0x0,0` + "\n" +
		`+QMBNCFG: "List",3,1,1,"hVoLTE-Verizon",0x05010141,202111151` + "\n" +
		"OK\n"
	if !verizonMBNActiveFromAT(raw) {
		t.Fatal("expected hVoLTE-Verizon selected+activated")
	}
	cdmaLess := "" +
		`+QMBNCFG: "List",3,1,1,"CDMAless-Verizon",0x050101B4,202312191` + "\n" +
		`+QMBNCFG: "List",11,0,0,"VoLTE_OPNMKT_CT",0x050113FC,202402281` + "\n" +
		"OK\n"
	if !verizonMBNActiveFromAT(cdmaLess) {
		t.Fatal("expected CDMAless-Verizon selected+activated")
	}
	ctActive := "" +
		`+QMBNCFG: "List",3,0,0,"CDMAless-Verizon",0x050101B4,202312191` + "\n" +
		`+QMBNCFG: "List",11,1,1,"VoLTE_OPNMKT_CT",0x050113FC,202402281` + "\n" +
		"OK\n"
	if verizonMBNActiveFromAT(ctActive) {
		t.Fatal("CT-active list must not set Verizon MBN flag")
	}
	if verizonMBNActiveFromAT(`+QMBNCFG: "List",3,0,0,"hVoLTE-Verizon",0x05010141,202111151` + "\nOK\n") {
		t.Fatal("inactive MBN must not set flag")
	}
	if verizonMBNActiveFromAT(`+QMBNCFG: "List",1,1,1,"Commercial-TMO",0x1,1` + "\nOK\n") {
		t.Fatal("non-Verizon MBN must not set flag")
	}
}

func TestNetworkRegisteredFromAT(t *testing.T) {
	if !networkRegisteredFromAT("+CEREG: 0,1\nOK\n") {
		t.Fatal("CEREG home")
	}
	if !networkRegisteredFromAT("+CREG: 2,5,\"5F0C\",\"E0B70B\",7\nOK\n") {
		t.Fatal("CREG roaming")
	}
	if networkRegisteredFromAT("+CEREG: 0,2\n+CREG: 0,2\nOK\n") {
		t.Fatal("searching must not count as registered")
	}
}
