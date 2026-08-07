package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"

	"google.golang.org/grpc"
	"gopkg.in/yaml.v3"

	"aipc/platform/common/constants"
	"aipc/platform/common/factoryeeprom"
	"aipc/platform/device-discovery/discovery"
	"aipc/platform/device-discovery/handler"
	pb "aipc/platform/device-discovery/proto"
)

var (
	listenAddr = flag.String("listen", "unix:///run/aipc/device-discovery.sock", "gRPC listen address")
	configFile = flag.String("config", "/data/aipc/etc/discovery.yaml", "config file path")
)

type Config struct {
	Discovery struct {
		MulticastAddr string `yaml:"multicast_addr"`
		MulticastPort int    `yaml:"multicast_port"`
		Timeout       int    `yaml:"timeout"`
		Interface     string `yaml:"interface"`
	} `yaml:"discovery"`

	Announce struct {
		Enabled     bool     `yaml:"enabled"`
		Product     string   `yaml:"product"`
		SN          string   `yaml:"sn"`
		Port        int      `yaml:"port"`
		Interval    int      `yaml:"interval"`
		Caps        []string `yaml:"caps"`
		VersionFile string   `yaml:"version_file"`
	} `yaml:"announce"`
}

func main() {
	flag.Parse()
	log.SetPrefix("[discovery] ")

	cfg := loadConfig(*configFile)

	registry := discovery.NewRegistry()

	var announcer *discovery.Announcer

	// Disable rp_filter so multicast from any source IP is accepted.
	// When device IP changes to a different subnet, the kernel's reverse
	// path filter drops incoming multicast from the old subnet.
	if err := os.WriteFile("/proc/sys/net/ipv4/conf/eth0/rp_filter", []byte("0"), 0644); err != nil {
		log.Printf("WARNING: failed to set rp_filter=0: %v", err)
	}
	// Persist to sysctl.conf so it survives reboots
	_ = os.WriteFile("/etc/sysctl.d/99-rp_filter.conf", []byte("net.ipv4.conf.eth0.rp_filter=0\n"), 0644)

	listener, err := discovery.NewListener(registry)
	if err != nil {
		log.Fatalf("failed to create multicast listener: %v", err)
	}
	defer listener.Close()

	go listener.Listen()
	go listener.WatchInterfaces("eth0")
	go timeoutChecker(registry)

	mySN := resolveSN(cfg.Announce.SN, cfg.Announce.VersionFile)
	myMAC := detectMAC("eth0")
	listener.OnSetNetwork = func(cfg discovery.SetNetwork) {
		if !matchesSetNetworkTarget(cfg, mySN, myMAC) {
			log.Printf("[discovery] set-network not for us: got SN=%s MAC=%s, mine SN=%s MAC=%s", cfg.SN, cfg.MAC, mySN, myMAC)
			return
		}
		log.Printf("[discovery] applying network config: %+v", cfg)
		if err := applyNetworkConfig(cfg); err != nil {
			log.Printf("[discovery] network config failed: %v", err)
		} else {
			log.Printf("[discovery] network config applied successfully")
			if announcer != nil {
				announcer.Refresh()
			}
			if err := listener.Rebind(); err != nil {
				log.Printf("[discovery] listener rebind failed: %v", err)
			}
		}
	}

	if cfg.Announce.Enabled {
		interval := time.Duration(cfg.Announce.Interval) * time.Second
		if interval < 1*time.Second {
			interval = 5 * time.Second
		}
		announcer = discovery.NewAnnouncer(discovery.AnnouncerConfig{
			Product:   cfg.Announce.Product,
			SN:        resolveSN(cfg.Announce.SN, cfg.Announce.VersionFile),
			FW:        readFirmwareVersion(cfg.Announce.VersionFile),
			Port:      cfg.Announce.Port,
			Caps:      cfg.Announce.Caps,
			Interface: cfg.Discovery.Interface,
			Interval:  interval,
		})
		ctx, cancel := context.WithCancel(context.Background())
		defer cancel()
		if err := announcer.Start(ctx); err != nil {
			log.Printf("WARNING: announcer failed to start: %v", err)
		} else {
			log.Printf("announcer started: %s every %s", cfg.Announce.Product, interval)
		}
	}

	grpcServer := grpc.NewServer()
	pb.RegisterDiscoveryServiceServer(grpcServer, handler.NewHandler(registry))

	lis, err := net.Listen("unix", parseAddr(*listenAddr))
	if err != nil {
		log.Fatalf("failed to listen: %v", err)
	}

	go func() {
		log.Printf("gRPC server listening on %s", *listenAddr)
		if err := grpcServer.Serve(lis); err != nil {
			log.Fatalf("gRPC serve failed: %v", err)
		}
	}()

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM, syscall.SIGUSR1)
	for s := range sig {
		if s == syscall.SIGUSR1 {
			log.Println("[discovery] SIGUSR1 received, forcing listener rebind")
			if err := listener.Rebind(); err != nil {
				log.Printf("[discovery] SIGUSR1 rebind failed: %v", err)
			}
			if announcer != nil {
				announcer.Refresh()
			}
			continue
		}
		break
	}
	log.Println("shutting down...")
	grpcServer.GracefulStop()
}

// matchesSetNetworkTarget selects exactly one device for a network change.
// A supplied MAC is authoritative and must not fall back to SN on mismatch,
// because cloned devices can share the same serial number.
func matchesSetNetworkTarget(cfg discovery.SetNetwork, mySN, myMAC string) bool {
	targetMAC := strings.TrimSpace(cfg.MAC)
	if targetMAC != "" {
		localMAC := strings.TrimSpace(myMAC)
		return localMAC != "" && strings.EqualFold(targetMAC, localMAC)
	}

	targetSN := strings.TrimSpace(cfg.SN)
	return targetSN != "" && targetSN == strings.TrimSpace(mySN)
}

func loadConfig(path string) *Config {
	cfg := &Config{}
	data, err := os.ReadFile(path)
	if err != nil {
		log.Printf("config not found (%s), using defaults", path)
		cfg.Announce.Enabled = true
		cfg.Announce.Product = "NE503"
		cfg.Announce.Port = 8080
		cfg.Announce.Interval = 5
		cfg.Announce.Caps = []string{"ai", "camera", "http", "mqtt"}
		cfg.Announce.VersionFile = filepath.Join(constants.RootPath(), "VERSION")
		return cfg
	}
	if err := yaml.Unmarshal(data, cfg); err != nil {
		log.Printf("config parse error (%s): %v", path, err)
	}
	// Set defaults for empty fields
	if cfg.Announce.VersionFile == "" {
		cfg.Announce.VersionFile = filepath.Join(constants.RootPath(), "VERSION")
	}
	return cfg
}

func resolveSN(cfgSN, versionFile string) string {
	if cfgSN != "" {
		return cfgSN
	}
	if info, err := factoryeeprom.DefaultClient().Read(context.Background()); err == nil {
		if serial := strings.TrimSpace(info.SerialNumber); serial != "" {
			return serial
		}
	}
	if data, err := os.ReadFile(versionFile); err == nil {
		for _, line := range strings.Split(string(data), "\n") {
			if strings.HasPrefix(line, "serial=") {
				return strings.TrimPrefix(line, "serial=")
			}
		}
	}
	hostname, _ := os.Hostname()
	if hostname != "" {
		return hostname
	}
	return "unknown"
}

func readFirmwareVersion(versionFile string) string {
	if data, err := os.ReadFile(versionFile); err == nil {
		for _, line := range strings.Split(string(data), "\n") {
			if strings.HasPrefix(line, "version=") {
				return strings.TrimPrefix(line, "version=")
			}
		}
	}
	return "unknown"
}

func detectMAC(ifaceName string) string {
	iface, err := net.InterfaceByName(ifaceName)
	if err != nil {
		return ""
	}
	return iface.HardwareAddr.String()
}

func timeoutChecker(registry *discovery.Registry) {
	ticker := time.NewTicker(10 * time.Second)
	defer ticker.Stop()
	for range ticker.C {
		expired := registry.CheckTimeouts()
		for _, d := range expired {
			log.Printf("device %s (%s) went OFFLINE", d.SerialNumber, d.Product)
		}
	}
}

func parseAddr(addr string) string {
	if len(addr) > 7 && addr[:7] == "unix://" {
		return addr[7:]
	}
	return addr
}

var (
	applyMu      sync.Mutex
	lastApplyKey string
)

// staticTargetAlreadySet reports whether the target static address is already
// present on the interface. Used by the dedup path so a re-send is forced when
// the actual IP has drifted from lastApplyKey (which is in-memory only).
func staticTargetAlreadySet(cfg discovery.SetNetwork) bool {
	iface := cfg.Interface
	if iface == "" {
		iface = "eth0"
	}
	if cfg.IPAddress == "" || cfg.SubnetMask == "" {
		return false
	}
	want := fmt.Sprintf("%s/%d", cfg.IPAddress, maskToCIDR(cfg.SubnetMask))
	for _, a := range listInterfaceIPv4(iface) {
		if a == want {
			return true
		}
	}
	return false
}

func applyNetworkConfig(cfg discovery.SetNetwork) error {
	// Deduplicate: skip if the same config was already applied. For static
	// mode we additionally verify the target IP is actually on the interface:
	// lastApplyKey is in-memory only and can drift from reality if the IP was
	// overwritten after the last apply (systemd-networkd/dhclient reload, or a
	// first attempt that set lastApplyKey before failing). When they diverge
	// we force a re-apply — otherwise the device is stuck on the wrong IP and
	// every re-send is silently swallowed as a "duplicate".
	key := fmt.Sprintf("%s:%s:%s:%s", cfg.SN, cfg.Mode, cfg.IPAddress, cfg.SubnetMask)
	applyMu.Lock()
	dup := key == lastApplyKey
	if !dup {
		lastApplyKey = key
	}
	applyMu.Unlock()

	if dup {
		if cfg.Mode == "static" && !staticTargetAlreadySet(cfg) {
			log.Printf("[discovery] re-applying (actual IP differs from target): %s", key)
		} else {
			log.Printf("[discovery] skipping duplicate applyNetworkConfig: %s", key)
			return nil
		}
	}

	iface := cfg.Interface
	if iface == "" {
		iface = "eth0"
	}

	switch cfg.Mode {
	case "dhcp":
		if err := runCmd("ip", "addr", "flush", "dev", iface); err != nil {
			log.Printf("[discovery] flush addr warning: %v", err)
		}
		if err := runCmd("dhclient", "-r", iface); err != nil {
			log.Printf("[discovery] dhclient release warning: %v", err)
		}
		if err := runCmd("dhclient", iface); err != nil {
			return fmt.Errorf("dhclient failed: %w", err)
		}
		log.Printf("[discovery] DHCP enabled on %s", iface)

	case "static":
		if cfg.IPAddress == "" || cfg.SubnetMask == "" {
			return fmt.Errorf("static mode requires ip_address and subnet_mask")
		}

		cidr := maskToCIDR(cfg.SubnetMask)
		newAddr := fmt.Sprintf("%s/%d", cfg.IPAddress, cidr)

		// Snapshot addresses BEFORE adding the new one, so we only
		// delete addresses that existed prior to this config change.
		preExisting := listInterfaceIPv4(iface)

		// Add new address FIRST — preserves IGMP membership on old addr
		// so multicast listener keeps receiving during the transition.
		if err := runCmd("ip", "addr", "add", newAddr, "dev", iface); err != nil {
			runCmd("ip", "addr", "del", newAddr, "dev", iface)
			if err := runCmd("ip", "addr", "add", newAddr, "dev", iface); err != nil {
				return fmt.Errorf("set ip failed: %w", err)
			}
		}

		// Remove old addresses immediately. Only remove addresses that
		// existed BEFORE we added the new one.
		for _, addr := range preExisting {
			if addr != newAddr {
				log.Printf("[discovery] removing old address %s", addr)
				runCmd("ip", "addr", "del", addr, "dev", iface)
			}
		}

		// Set gateway
		if cfg.Gateway != "" {
			if err := runCmd("ip", "route", "replace", "default", "via", cfg.Gateway, "dev", iface); err != nil {
				log.Printf("[discovery] set gateway warning: %v", err)
			}
		}

		// Write resolv.conf for DNS
		if err := writeResolvConf(cfg.DNS1, cfg.DNS2); err != nil {
			log.Printf("[discovery] write resolv.conf warning: %v", err)
		}

		log.Printf("[discovery] static IP applied: %s/%d on %s, gw=%s", cfg.IPAddress, cidr, iface, cfg.Gateway)

	default:
		return fmt.Errorf("unknown mode: %s", cfg.Mode)
	}

	// Persist config for next boot and sync with systemd-networkd
	writeNetworkConfigFile(cfg)
	if err := writeSystemdNetworkFile(cfg); err != nil {
		log.Printf("[discovery] systemd-network file warning: %v", err)
	}
	if err := reloadSystemdNetwork(); err != nil {
		log.Printf("[discovery] systemd-networkd reload warning: %v", err)
	}

	return nil
}

func runCmd(name string, args ...string) error {
	cmd := exec.Command(name, args...)
	output, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("%s %s: %s (%w)", name, strings.Join(args, " "), strings.TrimSpace(string(output)), err)
	}
	return nil
}

func maskToCIDR(mask string) int {
	ip := net.ParseIP(mask)
	if ip == nil {
		return 24
	}
	ones, _ := net.IPv4Mask(ip[12], ip[13], ip[14], ip[15]).Size()
	return ones
}

func writeResolvConf(dns1, dns2 string) error {
	var lines []string
	if dns1 != "" {
		lines = append(lines, "nameserver "+dns1)
	}
	if dns2 != "" {
		lines = append(lines, "nameserver "+dns2)
	}
	if len(lines) == 0 {
		return nil
	}
	return os.WriteFile("/etc/resolv.conf", []byte(strings.Join(lines, "\n")+"\n"), 0644)
}

func writeNetworkConfigFile(cfg discovery.SetNetwork) {
	data, err := yaml.Marshal(map[string]interface{}{
		"interface":   cfg.Interface,
		"mode":        cfg.Mode,
		"ip_address":  cfg.IPAddress,
		"subnet_mask": cfg.SubnetMask,
		"gateway":     cfg.Gateway,
		"dns1":        cfg.DNS1,
		"dns2":        cfg.DNS2,
	})
	if err != nil {
		return
	}
	networkFile := filepath.Join(constants.RootPath(), "etc", "network.yaml")
	if err := os.MkdirAll(filepath.Dir(networkFile), 0755); err != nil {
		log.Printf("writeNetworkConfigFile: mkdir failed: %v", err)
		return
	}
	if err := os.WriteFile(networkFile, data, 0644); err != nil {
		log.Printf("writeNetworkConfigFile: write failed: %v", err)
	}
}

func writeSystemdNetworkFile(cfg discovery.SetNetwork) error {
	iface := cfg.Interface
	if iface == "" {
		iface = "eth0"
	}

	var sb strings.Builder
	sb.WriteString("[Match]\nName=" + iface + "\n\n")
	sb.WriteString("[Network]\n")
	switch cfg.Mode {
	case "dhcp":
		sb.WriteString("DHCP=yes\n")
	case "static":
		cidr := maskToCIDR(cfg.SubnetMask)
		sb.WriteString(fmt.Sprintf("Address=%s/%d\n", cfg.IPAddress, cidr))
		if cfg.Gateway != "" {
			sb.WriteString("Gateway=" + cfg.Gateway + "\n")
		}
		if cfg.DNS1 != "" {
			sb.WriteString("DNS=" + cfg.DNS1 + "\n")
		}
		if cfg.DNS2 != "" {
			sb.WriteString("DNS=" + cfg.DNS2 + "\n")
		}
	}

	path := "/etc/systemd/network/10-" + iface + ".network"
	return os.WriteFile(path, []byte(sb.String()), 0644)
}

func reloadSystemdNetwork() error {
	if err := runCmd("networkctl", "reload"); err != nil {
		return fmt.Errorf("networkctl reload: %w", err)
	}
	return nil
}

func listInterfaceIPv4(iface string) []string {
	if netIface, err := net.InterfaceByName(iface); err == nil {
		addrs, _ := netIface.Addrs()
		var result []string
		for _, addr := range addrs {
			if ipnet, ok := addr.(*net.IPNet); ok && ipnet.IP.To4() != nil {
				result = append(result, ipnet.String())
			}
		}
		return result
	}
	return nil
}
