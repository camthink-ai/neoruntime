package handlers

import "testing"

func TestParseModemIdentityEM05(t *testing.T) {
	raw := "" +
		"ATI\nQuectel\nEM05-G\nRevision: EM05GFAR07A07M1G\n\nOK\n" +
		"AT+CPIN?\n+CPIN: READY\n\nOK\n" +
		"AT+CGSN\n869283051036293\n\nOK\n" +
		"AT+CIMI\n460115211256250\n\nOK\n" +
		"AT+CCID\n+CCID: 89860324245920230936\n\nOK\n" +
		"AT+COPS?\n+COPS: 0,0,\"CHN-CT\",7\n\nOK\n" +
		"AT+CEREG?\n+CEREG: 0,1\n\nOK\n" +
		"AT+QCSQ\n+QCSQ: \"LTE\",67,-97,166,-11\n\nOK\n" +
		"AT+CNUM\n+CNUM: ,\"+8617706006579\",145\n\nOK\n"

	id := parseModemIdentity(raw)
	if !id.OK || !id.SIMReady {
		t.Fatalf("ready: %+v", id)
	}
	if id.IMEI != "869283051036293" {
		t.Fatalf("imei=%q", id.IMEI)
	}
	if id.IMSI != "460115211256250" {
		t.Fatalf("imsi=%q", id.IMSI)
	}
	if id.ICCID != "89860324245920230936" {
		t.Fatalf("iccid=%q", id.ICCID)
	}
	if id.Operator != "CHN-CT" {
		t.Fatalf("operator=%q", id.Operator)
	}
	if id.Network != "LTE" {
		t.Fatalf("network=%q", id.Network)
	}
	if id.MSISDN != "+8617706006579" {
		t.Fatalf("msisdn=%q", id.MSISDN)
	}
	if id.Firmware != "EM05GFAR07A07M1G" {
		t.Fatalf("fw=%q", id.Firmware)
	}
	if id.RegState != "registered" {
		t.Fatalf("reg=%q", id.RegState)
	}
	if id.SignalRSRP == nil || *id.SignalRSRP != -97 {
		t.Fatalf("rsrp=%v", id.SignalRSRP)
	}
	if id.SignalRSRQ == nil || *id.SignalRSRQ != -11 {
		t.Fatalf("rsrq=%v", id.SignalRSRQ)
	}
}

func TestCeregState(t *testing.T) {
	if ceregState("5") != "roaming" {
		t.Fatal(ceregState("5"))
	}
}

func TestParseModemIdentityIMSINotIMEI(t *testing.T) {
	// When CIMI fails, IMSI must stay empty — not reuse CGSN/IMEI digits.
	raw := "" +
		"AT+CPIN?\n+CPIN: READY\n\nOK\n" +
		"AT+CGSN\n869283051036293\n\nOK\n" +
		"AT+CIMI\nERROR\n" +
		"AT+CCID\n+CCID: 89860324245920230936\n\nOK\n"
	id := parseModemIdentity(raw)
	if id.IMEI != "869283051036293" {
		t.Fatalf("imei=%q", id.IMEI)
	}
	if id.IMSI != "" {
		t.Fatalf("imsi should be empty, got %q", id.IMSI)
	}
}

func TestParseModemIdentityPrefixedCGSNCIMI(t *testing.T) {
	raw := "" +
		"AT+CPIN?\n+CPIN: READY\n\nOK\n" +
		"AT+CGSN\n+CGSN: 869283051036293\n\nOK\n" +
		"AT+CIMI\n+CIMI: 460115211256250\n\nOK\n" +
		"AT+CCID\n+CCID: 89860324245920230936\n\nOK\n"
	id := parseModemIdentity(raw)
	if id.IMEI != "869283051036293" {
		t.Fatalf("imei=%q", id.IMEI)
	}
	if id.IMSI != "460115211256250" {
		t.Fatalf("imsi=%q", id.IMSI)
	}
}

func TestParseModemIdentitySIMAbsentCME10(t *testing.T) {
	raw := "" +
		"AT+CPIN?\n+CME ERROR: 10\n" +
		"AT+CGSN\n869283051036293\n\nOK\n" +
		"AT+CIMI\nERROR\n" +
		"AT+CCID\n+CME ERROR: 13\n"
	id := parseModemIdentity(raw)
	if id.SIMReady {
		t.Fatalf("SIM should not be ready: %+v", id)
	}
	if id.IMEI != "869283051036293" {
		t.Fatalf("imei=%q", id.IMEI)
	}
	if id.IMSI != "" || id.ICCID != "" {
		t.Fatalf("SIM fields should be empty: imsi=%q iccid=%q", id.IMSI, id.ICCID)
	}
}

func TestParseModemIdentityEchoedCGSN(t *testing.T) {
	// atExchange injects the command name, then the modem may also echo it
	// when ATE0 has not taken effect yet — digits must still be found.
	raw := "" +
		"AT+CPIN?\n+CPIN: READY\n\nOK\n" +
		"AT+CGSN\nAT+CGSN\r\n869283051036293\r\n\r\nOK\r\n\n" +
		"AT+CIMI\nAT+CIMI\r\n460115211256250\r\n\r\nOK\r\n\n" +
		"AT+CCID\n+CCID: 89860324245920230936\n\nOK\n"
	id := parseModemIdentity(raw)
	if id.IMEI != "869283051036293" {
		t.Fatalf("imei=%q", id.IMEI)
	}
	if id.IMSI != "460115211256250" {
		t.Fatalf("imsi=%q", id.IMSI)
	}
}

func TestParseCPINResponse(t *testing.T) {
	ready, ok := parseCPINResponse("AT+CPIN?\n+CPIN: READY\n\nOK\n")
	if !ok || !ready {
		t.Fatalf("ready want true,true got %v,%v", ready, ok)
	}
	ready, ok = parseCPINResponse("AT+CPIN?\n+CPIN: SIM PIN\n\nOK\n")
	if !ok || ready {
		t.Fatalf("pin want false,true got %v,%v", ready, ok)
	}
	ready, ok = parseCPINResponse("AT+CPIN?\n+CME ERROR: 13\n")
	if !ok || ready {
		t.Fatalf("cme13 want false,true got %v,%v", ready, ok)
	}
	ready, ok = parseCPINResponse("AT+CPIN?\n+CME ERROR: 10\n")
	if !ok || ready {
		t.Fatalf("cme10 want false,true got %v,%v", ready, ok)
	}
	_, ok = parseCPINResponse("")
	if ok {
		t.Fatal("empty should not be ok")
	}
}
