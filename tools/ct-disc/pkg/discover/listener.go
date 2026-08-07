package discover

import (
	"context"
	"fmt"
	"log"
	"net"
	"time"

	"golang.org/x/net/ipv4"
)

type DeviceEvent struct {
	Type   string // "online", "update", "offline"
	Device *DeviceInfo
}

type ListenerStats struct {
	RecvCount  int
	DecodeErrs int
	EventCount int
	Running    bool
	Ifaces     []string
}

type Listener struct {
	conn      *net.UDPConn
	pconn     *ipv4.PacketConn
	registry  *Registry
	ifaceName string
	ctx       context.Context
	cancel    context.CancelFunc
	Events    chan DeviceEvent
	Verbose   bool
	stats     ListenerStats
}

func NewListener(registry *Registry, ifaceName string) (*Listener, error) {
	return NewListenerWithContext(context.Background(), registry, ifaceName)
}

func NewListenerWithContext(ctx context.Context, registry *Registry, ifaceName string) (*Listener, error) {
	childCtx, cancel := context.WithCancel(ctx)

	l := &Listener{
		registry:  registry,
		ifaceName: ifaceName,
		ctx:       childCtx,
		cancel:    cancel,
		Events:    make(chan DeviceEvent, 64),
	}

	// Bind to 0.0.0.0:port — works reliably on Windows and Linux
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{
		IP:   net.ParseIP("0.0.0.0"),
		Port: MulticastPort,
	})
	if err != nil {
		cancel()
		return nil, fmt.Errorf("failed to listen on :%d: %w", MulticastPort, err)
	}
	l.conn = conn

	// Use ipv4.PacketConn for reliable cross-platform multicast join
	pconn := ipv4.NewPacketConn(conn)
	l.pconn = pconn

	group := net.ParseIP(MulticastAddr)

	if ifaceName != "" {
		iface, err := net.InterfaceByName(ifaceName)
		if err != nil {
			cancel()
			conn.Close()
			return nil, fmt.Errorf("interface %s not found: %w", ifaceName, err)
		}
		if err := pconn.JoinGroup(iface, &net.UDPAddr{IP: group}); err != nil {
			log.Printf("[discover] JoinGroup on %s failed: %v, trying all interfaces\n", ifaceName, err)
		} else {
			l.stats.Ifaces = []string{ifaceName}
			log.Printf("[discover] joined multicast on %s\n", ifaceName)
		}
	}

	// If no specific interface or join failed, join on all usable interfaces
	if len(l.stats.Ifaces) == 0 {
		ifaces, err := net.Interfaces()
		if err == nil {
			for _, iface := range ifaces {
				if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 {
					continue
				}
				addrs, err := iface.Addrs()
				if err != nil || len(addrs) == 0 {
					continue
				}
				if err := pconn.JoinGroup(&iface, &net.UDPAddr{IP: group}); err != nil {
					log.Printf("[discover] JoinGroup on %s failed: %v\n", iface.Name, err)
					continue
				}
				l.stats.Ifaces = append(l.stats.Ifaces, iface.Name)
				log.Printf("[discover] joined multicast on %s\n", iface.Name)
			}
		}
	}

	return l, nil
}

func (l *Listener) GetStats() ListenerStats {
	return l.stats
}

func (l *Listener) Listen() {
	l.stats.Running = true
	defer func() { l.stats.Running = false }()

	buf := make([]byte, MaxPacketSize)
	for {
		select {
		case <-l.ctx.Done():
			return
		default:
		}

		l.conn.SetReadDeadline(time.Now().Add(1 * time.Second))
		n, src, err := l.conn.ReadFromUDP(buf)
		if err != nil {
			if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
				continue
			}
			continue
		}

		l.stats.RecvCount++

		a, err := DecodeAnnounce(buf[:n])
		if err != nil {
			l.stats.DecodeErrs++
			if l.Verbose {
				log.Printf("[discover] decode error from %s: %v", src.IP, err)
			}
			continue
		}

		wasNew := false
		if _, exists := l.registry.GetByMAC(a.MAC); !exists {
			wasNew = true
		}

		dev := l.registry.Update(a)
		if l.Verbose {
			log.Printf("[discover] %s from %s (%s) at %s", a.Type, a.SN, a.Product, src.IP)
		}

		l.stats.EventCount++
		if wasNew {
			l.Events <- DeviceEvent{Type: "online", Device: dev}
		} else {
			l.Events <- DeviceEvent{Type: "update", Device: dev}
		}
	}
}

func (l *Listener) Close() error {
	l.cancel()
	if l.pconn != nil {
		l.pconn.Close()
	}
	if l.conn != nil {
		return l.conn.Close()
	}
	return nil
}

func SendProbe(ifaceName string) error {
	addr := MulticastUDPAddr()
	probe := Announce{Type: ProbeType}
	data, err := EncodeAnnounce(probe)
	if err != nil {
		return err
	}

	return sendMulticastAllInterfaces(ifaceName, data, addr)
}

func sendMulticastAllInterfaces(ifaceName string, data []byte, addr *net.UDPAddr) error {
	var ips []net.IP

	if ifaceName != "" {
		iface, err := net.InterfaceByName(ifaceName)
		if err != nil {
			return fmt.Errorf("interface %q not found: %w", ifaceName, err)
		}
		addrs, _ := iface.Addrs()
		for _, a := range addrs {
			if ipnet, ok := a.(*net.IPNet); ok && ipnet.IP.To4() != nil {
				ips = append(ips, ipnet.IP)
			}
		}
	} else {
		ifaces, err := net.Interfaces()
		if err != nil {
			return err
		}
		for _, iface := range ifaces {
			if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 || iface.Flags&net.FlagMulticast == 0 {
				continue
			}
			addrs, _ := iface.Addrs()
			for _, a := range addrs {
				if ipnet, ok := a.(*net.IPNet); ok && ipnet.IP.To4() != nil {
					ips = append(ips, ipnet.IP)
				}
			}
		}
	}

	if len(ips) == 0 {
		return fmt.Errorf("no multicast-capable interfaces found")
	}

	var lastErr error
	for _, srcIP := range ips {
		conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: srcIP, Port: 0})
		if err != nil {
			lastErr = err
			continue
		}
		_, err = conn.WriteToUDP(data, addr)
		if err != nil {
			lastErr = err
		}
		conn.Close()
	}
	return lastErr
}

func GetNetworkInterfaces() []string {
	ifaces, err := net.Interfaces()
	if err != nil {
		return nil
	}
	var names []string
	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		addrs, err := iface.Addrs()
		if err != nil || len(addrs) == 0 {
			continue
		}
		names = append(names, iface.Name)
	}
	return names
}
