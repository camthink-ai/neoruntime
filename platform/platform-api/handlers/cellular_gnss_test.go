package handlers

import "testing"

func TestParseQGPSState(t *testing.T) {
	active, ok := parseQGPSState("AT+QGPS?\n\n+QGPS: 1\n\nOK\n")
	if !ok || !active {
		t.Fatalf("want active=true ok=true")
	}
	active, ok = parseQGPSState("+QGPS: 0\r\nOK\r\n")
	if !ok || active {
		t.Fatalf("want active=false ok=true")
	}
}

func TestParseQGPSLOC(t *testing.T) {
	var st CellularGnssStatus
	raw := "AT+QGPSLOC=2\n+QGPSLOC: 085305.0,24.62389,118.02898,1.1,127.1,2,0.00,0.0,0.0,101126,07\nOK\n"
	if !parseQGPSLOC(raw, &st) {
		t.Fatal("parse failed")
	}
	if st.Fix != "2d" || st.Latitude == nil || st.Longitude == nil {
		t.Fatalf("fix=%s lat=%v lon=%v", st.Fix, st.Latitude, st.Longitude)
	}
	if *st.Latitude < 24.62 || *st.Latitude > 24.63 {
		t.Fatalf("lat=%v", *st.Latitude)
	}
	if *st.Longitude < 118.02 || *st.Longitude > 118.03 {
		t.Fatalf("lon=%v", *st.Longitude)
	}
	if st.Altitude == nil || *st.Altitude != 127.1 {
		t.Fatalf("alt=%v", st.Altitude)
	}
	if st.Hdop == nil || *st.Hdop != 1.1 {
		t.Fatalf("hdop=%v", st.Hdop)
	}
	if st.Satellites == nil || *st.Satellites != 7 {
		t.Fatalf("sats=%v", st.Satellites)
	}
}
