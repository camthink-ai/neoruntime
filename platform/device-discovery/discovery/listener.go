package discovery

import (
	"log"
	"net"
	"sync"
	"time"

	"golang.org/x/net/ipv4"

	pb "aipc/platform/device-discovery/proto"
)

type Listener struct {
	mu            sync.Mutex
	conn          *net.UDPConn
	registry      *Registry
	OnDeviceEvent func(device *pb.DiscoveredDevice, isNew bool)
	OnSetNetwork  func(cfg SetNetwork)
}

func NewListener(registry *Registry) (*Listener, error) {
	conn, err := joinMulticast("eth0")
	if err != nil {
		return nil, err
	}

	return &Listener{
		conn:     conn,
		registry: registry,
	}, nil
}

func (l *Listener) getConn() *net.UDPConn {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.conn
}

// WatchInterfaces periodically checks if the interface IP has changed
// and rebinds the multicast socket to refresh IGMP membership.
// This handles the case where IP is changed externally (e.g. ip addr add/del)
// and no OnSetNetwork callback fires.
func (l *Listener) WatchInterfaces(ifaceName string) {
	var lastIP string
	for {
		time.Sleep(3 * time.Second)
		currentIP := detectInterfaceIP(ifaceName)
		if currentIP == "" {
			continue
		}
		if lastIP == "" {
			lastIP = currentIP
			continue
		}
		if currentIP != lastIP {
			log.Printf("[discovery] interface IP changed: %s -> %s, rebinding listener", lastIP, currentIP)
			lastIP = currentIP
			if err := l.Rebind(); err != nil {
				log.Printf("[discovery] auto-rebind failed: %v", err)
			}
		}
	}
}

func detectInterfaceIP(ifaceName string) string {
	iface, err := net.InterfaceByName(ifaceName)
	if err != nil {
		return ""
	}
	addrs, err := iface.Addrs()
	if err != nil {
		return ""
	}
	for _, addr := range addrs {
		if ipnet, ok := addr.(*net.IPNet); ok && ipnet.IP.To4() != nil {
			return ipnet.IP.String()
		}
	}
	return ""
}

func (l *Listener) Listen() {
	buf := make([]byte, MaxPacketSize)
	for {
		conn := l.getConn()
		if conn == nil {
			log.Printf("[discovery] listener: no connection, waiting for rebind")
			return
		}

		n, src, err := conn.ReadFromUDP(buf)
		if err != nil {
			// Check if connection was replaced by Rebind()
			l.mu.Lock()
			currentConn := l.conn
			l.mu.Unlock()
			if currentConn != conn {
				// Rebind() replaced the connection, retry with new one
				continue
			}
			// Same connection, real error
			log.Printf("[discovery] read error: %v", err)
			continue
		}

		msgType := PeekMessageType(buf[:n])

		switch msgType {
		case SetNetworkType:
			cfg, err := DecodeSetNetwork(buf[:n])
			if err != nil {
				log.Printf("[discovery] invalid set-network from %s: %v", src.IP, err)
				continue
			}
			log.Printf("[discovery] ct-set-network for SN=%s from %s", cfg.SN, src.IP)
			if l.OnSetNetwork != nil {
				l.OnSetNetwork(cfg)
			}

		default:
			a, err := DecodeAnnounce(buf[:n])
			if err != nil {
				continue
			}

			dev, isNew := l.registry.Update(a)
			status := "UPDATED"
			if isNew {
				status = "NEW"
			}
			log.Printf("[discovery] %s device %s (%s) at %s", status, a.SN, a.Product, a.IP)

			if l.OnDeviceEvent != nil {
				l.OnDeviceEvent(dev, isNew)
			}
		}
	}
}

func (l *Listener) Close() error {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.conn != nil {
		return l.conn.Close()
	}
	return nil
}

// Rebind closes the existing multicast socket and creates a new one.
// Call after network configuration changes (ip addr flush breaks IGMP membership).
func (l *Listener) Rebind() error {
	l.mu.Lock()
	defer l.mu.Unlock()

	if l.conn != nil {
		l.conn.Close()
	}

	conn, err := joinMulticast("eth0")
	if err != nil {
		log.Printf("[discovery] listener rebind FAILED: %v", err)
		return err
	}
	l.conn = conn
	log.Printf("[discovery] listener rebound on eth0")
	return nil
}

// joinMulticast binds to 0.0.0.0:port so the socket receives BOTH
// multicast AND unicast packets. After an IP change the switch may
// stop forwarding multicast, but unicast always reaches the device.
func joinMulticast(ifaceName string) (*net.UDPConn, error) {
	group := MulticastUDPAddr()

	// Bind 0.0.0.0 — receives multicast + unicast + broadcast
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{
		IP:   net.ParseIP("0.0.0.0"),
		Port: group.Port,
	})
	if err != nil {
		return nil, err
	}

	// Join multicast group on eth0
	iface, err := net.InterfaceByName(ifaceName)
	if err != nil {
		log.Printf("[discovery] interface %s not found, skipping IGMP join", ifaceName)
		return conn, nil
	}
	pconn := ipv4.NewPacketConn(conn)
	if err := pconn.JoinGroup(iface, &net.UDPAddr{IP: group.IP}); err != nil {
		log.Printf("[discovery] IGMP join on %s failed: %v (unicast still works)", ifaceName, err)
	} else {
		log.Printf("[discovery] IGMP join on %s OK for %s", ifaceName, group.IP)
	}

	return conn, nil
}
