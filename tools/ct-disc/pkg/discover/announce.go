package discover

import (
	"encoding/json"
	"errors"
	"log"
	"net"
	"time"
)

const (
	MulticastAddr = "239.255.255.250"
	MulticastPort = 19850
	AnnounceType  = "ct-announce"
	ProbeType     = "ct-probe"
	SetNetworkType = "ct-set-network"
	MaxPacketSize = 1024
)

var ErrInvalidAnnounce = errors.New("invalid announce packet")

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
	if a.Type == "" {
		a.Type = AnnounceType
	}
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
		return a, ErrInvalidAnnounce
	}
	return a, nil
}

func MulticastUDPAddr() *net.UDPAddr {
	return &net.UDPAddr{
		IP:   net.ParseIP(MulticastAddr),
		Port: MulticastPort,
	}
}

// SendSetNetwork sends a ct-set-network command via unicast, multicast and
// broadcast to ensure delivery regardless of network topology.
func SendSetNetwork(cfg SetNetwork, targetIP string) error {
	data, err := EncodeSetNetwork(cfg)
	if err != nil {
		return err
	}

	ifaces, err := net.Interfaces()
	if err != nil {
		return err
	}

	var ips []net.IP
	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		if iface.Flags&net.FlagMulticast == 0 {
			continue
		}
		addrs, _ := iface.Addrs()
		for _, addr := range addrs {
			if ipnet, ok := addr.(*net.IPNet); ok && ipnet.IP.To4() != nil {
				ips = append(ips, ipnet.IP)
			}
		}
	}

	if len(ips) == 0 {
		return errors.New("no multicast-capable interfaces found")
	}

	addr := MulticastUDPAddr()
	var lastErr error
	multicastOK := false
	for i := 0; i < 3; i++ {
		if i > 0 {
			time.Sleep(200 * time.Millisecond)
		}

		// 1. Unicast to target
		if targetIP != "" {
			for _, srcIP := range ips {
				conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: srcIP, Port: 0})
				if err != nil {
					lastErr = err
					continue
				}
				targetAddr := &net.UDPAddr{IP: net.ParseIP(targetIP), Port: MulticastPort}
				n, err := conn.WriteToUDP(data, targetAddr)
				if err != nil {
					lastErr = err
					log.Printf("[set-network] unicast %s -> %s failed: %v", srcIP, targetIP, err)
				} else {
					lastErr = nil
					log.Printf("[set-network] unicast %d bytes %s -> %s", n, srcIP, targetIP)
				}
				conn.Close()
			}
		}

		// 2. Multicast send
		for _, srcIP := range ips {
			conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: srcIP, Port: 0})
			if err != nil {
				lastErr = err
				continue
			}
			n, err := conn.WriteToUDP(data, addr)
			if err != nil {
				lastErr = err
				log.Printf("[set-network] multicast from %s failed: %v", srcIP, err)
			} else {
				multicastOK = true
				log.Printf("[set-network] multicast %d bytes %s -> %s", n, srcIP, addr)
			}
			conn.Close()
		}

		// 3. Broadcast fallback
		if err := sendBroadcast(data, MulticastPort); err != nil {
			log.Printf("[set-network] broadcast warning: %v", err)
		}
	}

	// If multicast succeeded, the command was delivered — clear any leftover
	// unicast error (e.g. "no route to host" when device is on different subnet).
	if multicastOK {
		lastErr = nil
	}

	return lastErr
}

// sendBroadcast sends a limited broadcast (255.255.255.255) from EACH
// network interface by binding to its source IP.
func sendBroadcast(data []byte, port int) error {
	ifaces, err := net.Interfaces()
	if err != nil {
		return err
	}

	var lastErr error
	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		addrs, _ := iface.Addrs()
		for _, addr := range addrs {
			ipnet, ok := addr.(*net.IPNet)
			if !ok || ipnet.IP.To4() == nil {
				continue
			}
			conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: ipnet.IP, Port: 0})
			if err != nil {
				lastErr = err
				continue
			}
			rawConn, err := conn.SyscallConn()
			if err != nil {
				conn.Close()
				lastErr = err
				continue
			}
			var ctrlErr error
			if err := rawConn.Control(func(fd uintptr) {
				ctrlErr = setBroadcast(fd)
			}); err != nil {
				conn.Close()
				lastErr = err
				continue
			}
			if ctrlErr != nil {
				conn.Close()
				lastErr = ctrlErr
				continue
			}
			n, err := conn.WriteToUDP(data, &net.UDPAddr{
				IP:   net.IPv4bcast,
				Port: port,
			})
			if err != nil {
				lastErr = err
				log.Printf("[set-network] broadcast from %s (%s) failed: %v", ipnet.IP, iface.Name, err)
			} else {
				log.Printf("[set-network] broadcast %d bytes from %s (%s)", n, ipnet.IP, iface.Name)
			}
			conn.Close()
		}
	}
	return lastErr
}
