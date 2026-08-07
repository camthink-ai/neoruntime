package discover

import (
	"context"
	"fmt"
	"log"
	"net"
	"time"
)

type Announcer struct {
	announce Announce
	iface    string
	interval time.Duration
	ctx      context.Context
	cancel   context.CancelFunc
	conn     *net.UDPConn
}

func NewAnnouncer(a Announce, iface string, interval time.Duration) *Announcer {
	return &Announcer{
		announce: a,
		iface:    iface,
		interval: interval,
	}
}

func (an *Announcer) Start(ctx context.Context) error {
	childCtx, cancel := context.WithCancel(ctx)
	an.ctx = childCtx
	an.cancel = cancel

	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.ParseIP("0.0.0.0"), Port: 0})
	if err != nil {
		cancel()
		return fmt.Errorf("failed to create UDP socket: %w", err)
	}
	an.conn = conn

	an.announce.Type = AnnounceType
	if an.announce.IP == "" {
		an.announce.IP = an.getLocalIP()
	}
	if an.announce.MAC == "" {
		an.announce.MAC = an.getLocalMAC()
	}

	go an.loop()
	return nil
}

func (an *Announcer) loop() {
	addr := MulticastUDPAddr()
	ticker := time.NewTicker(an.interval)
	defer ticker.Stop()

	an.send(addr)
	for {
		select {
		case <-an.ctx.Done():
			return
		case <-ticker.C:
			an.send(addr)
		}
	}
}

func (an *Announcer) send(addr *net.UDPAddr) {
	data, err := EncodeAnnounce(an.announce)
	if err != nil {
		log.Printf("[announcer] encode error: %v", err)
		return
	}
	_, err = an.conn.WriteToUDP(data, addr)
	if err != nil {
		log.Printf("[announcer] send error: %v", err)
	}
	log.Printf("[announcer] sent: %s (%s) at %s:%d", an.announce.SN, an.announce.Product, an.announce.IP, an.announce.Port)
}

func (an *Announcer) Stop() {
	if an.cancel != nil {
		an.cancel()
	}
	if an.conn != nil {
		an.conn.Close()
	}
}

func (an *Announcer) getLocalIP() string {
	ifaces, err := net.Interfaces()
	if err != nil {
		return "0.0.0.0"
	}
	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		if an.iface != "" && iface.Name != an.iface {
			continue
		}
		addrs, _ := iface.Addrs()
		for _, addr := range addrs {
			if ipnet, ok := addr.(*net.IPNet); ok && ipnet.IP.To4() != nil {
				return ipnet.IP.String()
			}
		}
	}
	return "0.0.0.0"
}

func (an *Announcer) getLocalMAC() string {
	ifaces, err := net.Interfaces()
	if err != nil {
		return ""
	}
	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		if an.iface != "" && iface.Name != an.iface {
			continue
		}
		if iface.HardwareAddr != nil {
			return iface.HardwareAddr.String()
		}
	}
	return ""
}
