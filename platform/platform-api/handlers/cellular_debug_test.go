package handlers

import "testing"

func TestSanitizeATCommand(t *testing.T) {
	ok, err := sanitizeATCommand("  AT+CGDCONT? \n")
	if err != nil || ok != "AT+CGDCONT?" {
		t.Fatalf("got %q err=%v", ok, err)
	}
	if _, err := sanitizeATCommand("CGDCONT?"); err == nil {
		t.Fatal("must require AT prefix")
	}
	if _, err := sanitizeATCommand("AT\nAT"); err == nil {
		t.Fatal("must reject multiline")
	}
	if _, err := sanitizeATCommand(""); err == nil {
		t.Fatal("must reject empty")
	}
}

func TestResolveCellularPingIfaceRejectsNonCellular(t *testing.T) {
	h := &CellularHandler{hw: &CellularHWProfile{}}
	h.hw.Net.Ifaces = []string{"wwan0", "usb0"}
	if _, err := h.resolveCellularPingIface("eth0"); err == nil {
		t.Fatal("must reject eth0")
	}
	if _, err := h.resolveCellularPingIface(""); err == nil {
		t.Fatal("must require an up cellular session")
	}
}
