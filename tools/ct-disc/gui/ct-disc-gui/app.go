package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"time"

	"github.com/camthink/ct-disc/pkg/discover"
	"github.com/camthink/ct-disc/pkg/mqttclient"
	"github.com/wailsapp/wails/v2/pkg/runtime"
)

type ListenerStats struct {
	RecvCount  int      `json:"recvCount"`
	DecodeErrs int      `json:"decodeErrs"`
	EventCount int      `json:"eventCount"`
	Running    bool     `json:"running"`
	Ifaces     []string `json:"ifaces"`
}

type DeviceItem struct {
	SN        string   `json:"sn"`
	Product   string   `json:"product"`
	IP        string   `json:"ip"`
	Port      int      `json:"port"`
	FW        string   `json:"fw"`
	Caps      []string `json:"caps"`
	HW        string   `json:"hw"`
	MAC       string   `json:"mac"`
	Online    bool     `json:"online"`
	LastSeen  string   `json:"lastSeen"`
	FirstSeen string   `json:"firstSeen"`
}

type Settings struct {
	MQTTBroker string `json:"mqttBroker"`
	MQTTUser   string `json:"mqttUser"`
	MQTTPass   string `json:"mqttPass"`
	Interface  string `json:"interface"`
}

type App struct {
	ctx      context.Context
	registry *discover.Registry
	listener *discover.Listener
	mqttCfg  mqttclient.Config
	settings Settings
}

func NewApp() *App {
	return &App{
		registry: discover.NewRegistry(),
		settings: Settings{
			MQTTBroker: "tcp://localhost:1883",
		},
	}
}

func (a *App) startup(ctx context.Context) {
	a.ctx = ctx
	log.Println("[app] startup called")
}

// Ping is a simple diagnostic method to verify Go<->JS bindings work
func (a *App) Ping() string {
	log.Println("[app] Ping called")
	return "pong"
}

func (a *App) StartDiscovery(iface string) string {
	log.Printf("[app] StartDiscovery called, iface=%q\n", iface)

	if a.listener != nil {
		a.listener.Close()
		a.listener = nil
	}

	// try with specified interface first, then all interfaces
	listener, err := discover.NewListenerWithContext(a.ctx, a.registry, iface)
	if err != nil {
		log.Printf("[app] ListenMulticastUDP(%q) failed: %v, trying all interfaces...\n", iface, err)
		listener, err = discover.NewListenerWithContext(a.ctx, a.registry, "")
	}
	if err != nil {
		log.Printf("[app] StartDiscovery failed: %v\n", err)
		return fmt.Sprintf("ERROR: %v", err)
	}
	a.listener = listener

	go listener.Listen()

	go func() {
		for {
			select {
			case <-a.ctx.Done():
				return
			case evt, ok := <-listener.Events:
				if !ok {
					return
				}
				item := deviceToItem(evt.Device)
				log.Printf("[app] event: %s %s\n", evt.Type, item.SN)
				runtime.EventsEmit(a.ctx, "device:"+evt.Type, item)
			}
		}
	}()

	go func() {
		ticker := time.NewTicker(5 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-a.ctx.Done():
				return
			case <-ticker.C:
				expired := a.registry.CheckTimeouts(30 * time.Second)
				for _, sn := range expired {
					log.Printf("[app] timeout: %s\n", sn)
					runtime.EventsEmit(a.ctx, "device:offline", sn)
				}
			}
		}
	}()

	msg := fmt.Sprintf("OK: listening on %s:%d", discover.MulticastAddr, discover.MulticastPort)
	log.Println("[app]", msg)
	return msg
}

func (a *App) StopDiscovery() error {
	if a.listener != nil {
		err := a.listener.Close()
		a.listener = nil
		return err
	}
	return nil
}

func (a *App) GetDevices() []DeviceItem {
	devices := a.registry.List()
	items := make([]DeviceItem, len(devices))
	for i, d := range devices {
		items[i] = deviceToItem(d)
	}
	return items
}

func (a *App) GetListenerStats() ListenerStats {
	if a.listener == nil {
		return ListenerStats{}
	}
	s := a.listener.GetStats()
	return ListenerStats{
		RecvCount:  s.RecvCount,
		DecodeErrs: s.DecodeErrs,
		EventCount: s.EventCount,
		Running:    s.Running,
		Ifaces:     s.Ifaces,
	}
}

func (a *App) ScanDevices(iface string) string {
	log.Printf("[app] ScanDevices called, iface=%q\n", iface)
	err := discover.SendProbe(iface)
	if err != nil {
		log.Printf("[app] ScanDevices error: %v\n", err)
		return fmt.Sprintf("ERROR: %v", err)
	}
	log.Println("[app] probe sent")
	return "OK: probe sent"
}

func (a *App) SendCommand(sn, cmd, payload string) (string, error) {
	client, err := mqttclient.NewClient(a.mqttCfg)
	if err != nil {
		return "", fmt.Errorf("MQTT connect failed: %w", err)
	}
	defer client.Close()

	resp, err := client.SendCommand(sn, cmd, payload, 10*time.Second)
	if err != nil {
		return "", err
	}
	return resp.Payload, nil
}

func (a *App) GetNetworkInterfaces() []string {
	return discover.GetNetworkInterfaces()
}

func (a *App) SaveSettings(settingsJSON string) error {
	var s Settings
	if err := json.Unmarshal([]byte(settingsJSON), &s); err != nil {
		return fmt.Errorf("invalid settings: %w", err)
	}
	a.settings = s
	a.mqttCfg = mqttclient.Config{
		Broker:   s.MQTTBroker,
		Username: s.MQTTUser,
		Password: s.MQTTPass,
	}
	return nil
}

func (a *App) GetSettings() Settings {
	return a.settings
}

func (a *App) OpenInBrowser(url string) {
	runtime.BrowserOpenURL(a.ctx, url)
}

type NetworkConfig struct {
	Interface  string `json:"interface"`
	Mode       string `json:"mode"`
	IPAddress  string `json:"ip_address"`
	SubnetMask string `json:"subnet_mask"`
	Gateway    string `json:"gateway"`
	DNS1       string `json:"dns1"`
	DNS2       string `json:"dns2"`
	MACAddress string `json:"mac"`
	SN         string `json:"sn"`
}

type apiResponse struct {
	Code    int             `json:"code"`
	Message string          `json:"message"`
	Data    json.RawMessage `json:"data"`
}

func (a *App) GetDeviceNetworkConfig(ip string) (NetworkConfig, error) {
	url := fmt.Sprintf("http://%s:8080/api/v1/network/config", ip)
	client := &http.Client{Timeout: 3 * time.Second}
	resp, err := client.Get(url)
	if err != nil {
		return NetworkConfig{}, fmt.Errorf("Device connection failed: %w", err)
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return NetworkConfig{}, fmt.Errorf("Read response failed: %w", err)
	}

	var apiResp apiResponse
	if err := json.Unmarshal(body, &apiResp); err != nil {
		return NetworkConfig{}, fmt.Errorf("Parse response failed: %w", err)
	}
	if apiResp.Code != 0 {
		return NetworkConfig{}, fmt.Errorf("Device returned error: %s", apiResp.Message)
	}

	var cfg NetworkConfig
	if err := json.Unmarshal(apiResp.Data, &cfg); err != nil {
		return NetworkConfig{}, fmt.Errorf("Parse config failed: %w", err)
	}
	return cfg, nil
}

func (a *App) SetDeviceNetworkConfig(ip string, cfgJSON string) (string, error) {
	var cfg NetworkConfig
	if err := json.Unmarshal([]byte(cfgJSON), &cfg); err != nil {
		return "", fmt.Errorf("Invalid config: %w", err)
	}

	// Resolve SN and MAC from registry by IP (frontend may not have them)
	if a.registry != nil {
		for _, d := range a.registry.List() {
			if d.Announce.IP == ip {
				if cfg.SN == "" {
					cfg.SN = d.Announce.SN
				}
				if cfg.MACAddress == "" {
					cfg.MACAddress = d.Announce.MAC
				}
				break
			}
		}
	}

	if cfg.SN == "" {
		return "", fmt.Errorf("Cannot resolve device SN — ensure device is online")
	}

	log.Printf("[app] SetNetwork: ip=%s SN=%s MAC=%s mode=%s targetIP=%s",
		ip, cfg.SN, cfg.MACAddress, cfg.Mode, cfg.IPAddress)

	err := discover.SendSetNetwork(discover.SetNetwork{
		SN:         cfg.SN,
		MAC:        cfg.MACAddress,
		Interface:  cfg.Interface,
		Mode:       cfg.Mode,
		IPAddress:  cfg.IPAddress,
		SubnetMask: cfg.SubnetMask,
		Gateway:    cfg.Gateway,
		DNS1:       cfg.DNS1,
		DNS2:       cfg.DNS2,
	}, ip)
	if err != nil {
		return "", fmt.Errorf("Multicast send failed: %w", err)
	}
	return fmt.Sprintf("OK: (SN: %s, MAC: %s)", cfg.SN, cfg.MACAddress), nil
}

func deviceToItem(d *discover.DeviceInfo) DeviceItem {
	return DeviceItem{
		SN:        d.Announce.SN,
		Product:   d.Announce.Product,
		IP:        d.Announce.IP,
		Port:      d.Announce.Port,
		FW:        d.Announce.FW,
		Caps:      d.Announce.Caps,
		HW:        d.Announce.HW,
		MAC:       d.Announce.MAC,
		Online:    d.Online,
		LastSeen:  d.LastSeen.Format("2006-01-02 15:04:05"),
		FirstSeen: d.FirstSeen.Format("2006-01-02 15:04:05"),
	}
}
