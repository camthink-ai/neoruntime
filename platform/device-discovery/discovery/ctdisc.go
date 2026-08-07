package discovery

import (
	"encoding/json"
	"net"
)

const (
	MulticastAddr  = "239.255.255.250"
	MulticastPort  = 19850
	AnnounceType   = "ct-announce"
	SetNetworkType = "ct-set-network"
	ProbeType      = "ct-probe"
	MaxPacketSize  = 1024
)

type Announce struct {
	Type    string   `json:"type"`
	Product string   `json:"product"`
	SN      string   `json:"sn"`
	MAC     string   `json:"mac"`
	IP      string   `json:"ip"`
	FW      string   `json:"fw"`
	Port    int      `json:"port"`
	Caps    []string `json:"caps"`
	HW      string   `json:"hw"`
}

type SetNetwork struct {
	Type       string `json:"type"`
	SN         string `json:"sn"`
	MAC        string `json:"mac"`
	Interface  string `json:"interface"`
	Mode       string `json:"mode"`
	IPAddress  string `json:"ip_address"`
	SubnetMask string `json:"subnet_mask"`
	Gateway    string `json:"gateway"`
	DNS1       string `json:"dns1"`
	DNS2       string `json:"dns2"`
}

func EncodeAnnounce(a Announce) ([]byte, error) {
	a.Type = AnnounceType
	return json.Marshal(a)
}

func EncodeSetNetwork(s SetNetwork) ([]byte, error) {
	s.Type = SetNetworkType
	return json.Marshal(s)
}

func DecodeAnnounce(data []byte) (Announce, error) {
	var a Announce
	if err := json.Unmarshal(data, &a); err != nil {
		return a, err
	}
	if a.Type != AnnounceType {
		return a, ErrInvalidPacket
	}
	return a, nil
}

func DecodeSetNetwork(data []byte) (SetNetwork, error) {
	var s SetNetwork
	if err := json.Unmarshal(data, &s); err != nil {
		return s, err
	}
	if s.Type != SetNetworkType {
		return s, ErrInvalidPacket
	}
	return s, nil
}

// PeekMessageType returns the "type" field from a JSON payload without full decode.
func PeekMessageType(data []byte) string {
	var partial struct {
		Type string `json:"type"`
	}
	json.Unmarshal(data, &partial)
	return partial.Type
}

func MulticastUDPAddr() *net.UDPAddr {
	return &net.UDPAddr{
		IP:   net.ParseIP(MulticastAddr),
		Port: MulticastPort,
	}
}
