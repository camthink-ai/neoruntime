// Command onvif-device exposes the NE503 AIPC as an ONVIF Profile S device.
//
// It is a thin signalling service: it answers WS-Discovery (UDP multicast) so
// NVR/VMS clients can find it on the LAN, and serves the ONVIF Device and Media
// SOAP services over HTTP. GetStreamURI hands out the RTSP URIs served by
// camera-daemon (rtsp://<device-ip>:8554/<stream>); video never flows through
// this process.
//
// Usage:
//
//	onvif-device [--config /data/aipc/etc/onvif.yaml] [--iface eth0]
//
// On IP change (DHCP renewal) it rewrites the advertised RTSP URIs and XAddrs
// and re-announces via WS-Discovery. SIGUSR1 forces a re-announce.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"time"

	onvifserver "github.com/0x524a/onvif-go/server"

	"aipc/platform/onvif-device/config"
	"aipc/platform/onvif-device/identity"
	"aipc/platform/onvif-device/wsdiscovery"
)

var (
	configFile = flag.String("config", "/data/aipc/etc/onvif.yaml", "config file path")
	ifaceFlag  = flag.String("iface", "", "override the LAN interface for multicast + IP discovery")
)

const (
	soapTimeout       = 30 * time.Second
	ipStartupTries    = 10 // number of 1s attempts to resolve the LAN IP before giving up
	ipWatchInterval   = 10 * time.Second
	defaultVideoQ     = 80.0
	endpointURNPrefix = "urn:uuid:"
)

func main() {
	flag.Parse()

	cfg, err := config.Load(*configFile)
	if err != nil {
		log.Fatalf("[onvif] config load failed: %v", err)
	}
	if *ifaceFlag != "" {
		cfg.Network.Interface = *ifaceFlag
	}
	if !cfg.Service.Enabled {
		log.Printf("[onvif] service disabled by config; exiting")
		return
	}

	// Device identity: serial + firmware from factory EEPROM / VERSION file,
	// and a stable endpoint UUID derived from the serial.
	sn := identity.ResolveSN(cfg.Device.SerialNumber, cfg.VersionFile)
	fw := cfg.Device.FirmwareVersion
	if fw == "" {
		fw = identity.ReadFirmwareVersion(cfg.VersionFile)
	}
	endpointUUID := endpointURNPrefix + identity.DeviceUUID(sn)
	log.Printf("[onvif] device sn=%s fw=%s endpoint=%s", sn, fw, endpointUUID)

	// rp_filter must be 0 or multicast from a different subnet is dropped.
	identity.DisableRpFilter(cfg.Network.Interface)

	// Resolve the LAN IP (retry briefly while the interface comes up).
	ip := resolveIPWithRetries(cfg.Network.Interface)

	// Build and start the ONVIF SOAP server (Device + Media services).
	srv, err := onvifserver.New(buildServerConfig(cfg, sn, fw, ip))
	if err != nil {
		log.Fatalf("[onvif] server config failed: %v", err)
	}
	if ip != "" {
		applyStreamURIs(srv, cfg, ip)
	}

	// Arm the live-stream overlay: ONVIF metadata (codec/resolution/bitrate/fps/
	// gop) tracks camera-daemon's actual running encoder instead of static
	// onvif.yaml. Non-fatal if the socket is missing/down — enrichProfiles
	// falls back to the static values, so ONVIF never breaks.
	initLiveStreams(cfg.Camera.Socket)

	// WS-Discovery responder (Hello + ProbeMatch). Runs in parallel; failure
	// is non-fatal so the SOAP service still works without multicast.
	responder := wsdiscovery.New(buildDiscoveryConfig(cfg, endpointUUID, ip))

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	var ipMu sync.Mutex
	lastIP := ip
	refresh := func() {
		newIP, err := identity.ResolveLanIP(cfg.Network.Interface)
		if err != nil {
			log.Printf("[onvif] LAN IP resolve failed: %v", err)
			return
		}
		ipMu.Lock()
		unchanged := newIP == lastIP
		if !unchanged {
			lastIP = newIP
		}
		ipMu.Unlock()
		if unchanged {
			return
		}
		log.Printf("[onvif] LAN IP changed -> %s; updating URIs and re-announcing", newIP)
		applyStreamURIs(srv, cfg, newIP)
		// Keep the advertised host in sync so GetCapabilities/GetServices XAddrs
		// resolve to the new IP. A concurrent SOAP read of this field during the
		// rare IP transition is a benign torn read that self-corrects on retry.
		srv.GetConfig().Host = newIP
		responder.SetXAddrs([]string{deviceXAddr(newIP, cfg)})
		if err := responder.SendHello(); err != nil {
			log.Printf("[onvif] re-announce Hello failed: %v", err)
		}
	}

	go func() {
		if err := responder.Start(ctx); err != nil {
			log.Printf("[onvif] WS-Discovery responder stopped: %v", err)
		}
	}()

	// Periodically re-resolve the IP to catch DHCP renewals.
	go func() {
		t := time.NewTicker(ipWatchInterval)
		defer t.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-t.C:
				refresh()
			}
		}
	}()

	// Signal handling: SIGUSR1 forces a re-announce; SIGINT/SIGTERM stops.
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM, syscall.SIGUSR1)
	go func() {
		for s := range sig {
			if s == syscall.SIGUSR1 {
				log.Printf("[onvif] SIGUSR1 received, forcing re-announce")
				refresh()
				if err := responder.SendHello(); err != nil {
					log.Printf("[onvif] forced Hello failed: %v", err)
				}
				continue
			}
			log.Printf("[onvif] %s received, shutting down", s)
			cancel()
			return
		}
	}()

	log.Printf("[onvif] SOAP Device/Media service on :%d%s (rtsp=:%d, ip=%s)",
		cfg.Service.HTTPPort, cfg.Service.BasePath, cfg.RTSP.Port, ip)

	if err := startSOAPServer(ctx, srv, cfg); err != nil {
		log.Fatalf("[onvif] SOAP server stopped with error: %v", err)
	}
	log.Printf("[onvif] shutdown complete")
}

// buildServerConfig maps the service config onto the onvif-go server Config.
// PTZ/Imaging are disabled for Phase 1 (Streaming profile only). Host is the
// advertised LAN IP used to build XAddrs; the HTTP listener always binds 0.0.0.0
// (see startSOAPServer), decoupling bind from advertise so GetCapabilities does
// not fall back to "localhost" like the library does when Host == "0.0.0.0".
func buildServerConfig(cfg *config.Config, sn, fw, ip string) *onvifserver.Config {
	profiles := make([]onvifserver.ProfileConfig, 0, len(cfg.Profiles))
	for _, p := range cfg.Profiles {
		profiles = append(profiles, onvifserver.ProfileConfig{
			Token: p.Token,
			Name:  p.Name,
			VideoSource: onvifserver.VideoSourceConfig{
				Token:      "vs_" + p.Token,
				Name:       p.Name,
				Resolution: onvifserver.Resolution{Width: p.Width, Height: p.Height},
				Framerate:  p.FPS,
				Bounds:     onvifserver.Bounds{Width: p.Width, Height: p.Height},
			},
			VideoEncoder: onvifserver.VideoEncoderConfig{
				Encoding:   p.Codec,
				Resolution: onvifserver.Resolution{Width: p.Width, Height: p.Height},
				Quality:    defaultVideoQ,
				Framerate:  p.FPS,
				Bitrate:    p.Bitrate,
				GovLength:  p.FPS,
			},
		})
	}

	user, pass := resolveAuth(cfg)
	return &onvifserver.Config{
		Host:     ip, // advertised host for XAddrs; bind is always 0.0.0.0 in startSOAPServer
		Port:     cfg.Service.HTTPPort,
		BasePath: cfg.Service.BasePath,
		Timeout:  soapTimeout,
		DeviceInfo: onvifserver.DeviceInfo{
			Manufacturer:    cfg.Device.Manufacturer,
			Model:           cfg.Device.Model,
			FirmwareVersion: fw,
			SerialNumber:    sn,
			HardwareID:      cfg.Device.HardwareID,
		},
		Username:       user,
		Password:       pass,
		SupportPTZ:     false, // Phase 3
		SupportImaging: false, // Phase 2
		SupportEvents:  false, // Phase 4
		Profiles:       profiles,
	}
}

// buildDiscoveryConfig assembles the WS-Discovery responder configuration.
func buildDiscoveryConfig(cfg *config.Config, endpointUUID, ip string) wsdiscovery.Config {
	return wsdiscovery.Config{
		EndpointUUID:  endpointUUID,
		Types:         "dp0:NetworkVideoTransmitter",
		Scopes:        cfg.Device.Scopes,
		XAddrs:        []string{deviceXAddr(ip, cfg)},
		Interface:     cfg.Network.Interface,
		MulticastAddr: cfg.Network.MulticastAddr,
		MulticastPort: cfg.Network.MulticastPort,
	}
}

// resolveAuth returns the WS-Security credentials, or empty for "none" mode.
// In "digest" mode it prefers AIPC_AUTH_USERNAME/AIPC_AUTH_PASSWORD (shared
// with platform-api) and falls back to none with a warning if unset.
func resolveAuth(cfg *config.Config) (string, string) {
	if strings.ToLower(cfg.Auth.Mode) != "digest" {
		return "", ""
	}
	user := envOr("AIPC_AUTH_USERNAME", cfg.Auth.Username)
	pass := envOr("AIPC_AUTH_PASSWORD", cfg.Auth.Password)
	if user == "" || pass == "" {
		log.Printf("[onvif] auth.mode=digest but credentials missing; running unauthenticated for interop")
		return "", ""
	}
	return user, pass
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

// applyStreamURIs overrides each profile's RTSP URI so the advertised address
// is the real device LAN IP (the library otherwise defaults to localhost).
func applyStreamURIs(srv *onvifserver.Server, cfg *config.Config, ip string) {
	for _, p := range cfg.Profiles {
		uri := rtspURI(ip, cfg, p)
		if err := srv.UpdateStreamURI(p.Token, uri); err != nil {
			log.Printf("[onvif] UpdateStreamURI(%s) failed: %v", p.Token, err)
			continue
		}
		log.Printf("[onvif] profile %s -> %s", p.Token, uri)
	}
}

func rtspURI(ip string, cfg *config.Config, p config.ProfileConfig) string {
	return fmt.Sprintf("rtsp://%s:%d/%s", ip, cfg.RTSP.Port, p.Stream)
}

func deviceXAddr(ip string, cfg *config.Config) string {
	return fmt.Sprintf("http://%s:%d%s/device_service", ip, cfg.Service.HTTPPort, cfg.Service.BasePath)
}

// resolveIPWithRetries waits up to ipStartupTries seconds for the interface to
// acquire an IPv4 address. Returns "" if it never comes up (the watcher keeps
// trying after start).
func resolveIPWithRetries(iface string) string {
	for range ipStartupTries {
		if ip, err := identity.ResolveLanIP(iface); err == nil {
			return ip
		}
		time.Sleep(time.Second)
	}
	log.Printf("[onvif] WARNING: no LAN IP on %s after %d tries; will retry in background", iface, ipStartupTries)
	return ""
}

// startSOAPServer runs the ONVIF Device + Media SOAP services over HTTP.
//
// We deliberately do not call onvifserver.Server.Start: it hardcodes the
// media-service action keys as "GetStreamURI"/"GetSnapshotURI" (capital URI) and
// tags the responses GetStreamURIResponse. The ONVIF Media WSDL spells these
// GetStreamUri/GetSnapshotUri (lowercase Uri), so strict clients (ONVIF Device
// Manager, gSOAP-generated NVR stubs) never match and GetStreamUri faults with
// "No handler for action". registerMediaRoutes re-registers the handlers under
// the spec-correct keys and retypes the responses so the wire output conforms.
//
// The listener binds 0.0.0.0; Config.Host carries only the advertised host used
// to build XAddrs (set to the real LAN IP in buildServerConfig/refresh).
func startSOAPServer(ctx context.Context, srv *onvifserver.Server, cfg *config.Config) error {
	mux := http.NewServeMux()
	registerDeviceRoutes(mux, srv)
	registerMediaRoutes(mux, srv)

	addr := fmt.Sprintf("0.0.0.0:%d", cfg.Service.HTTPPort)
	httpServer := &http.Server{
		Addr:         addr,
		Handler:      mux,
		ReadTimeout:  soapTimeout,
		WriteTimeout: soapTimeout,
	}

	errChan := make(chan error, 1)
	go func() {
		log.Printf("[onvif] SOAP listening on %s", addr)
		if err := httpServer.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			errChan <- err
		}
	}()

	select {
	case <-ctx.Done():
		log.Printf("[onvif] SOAP shutting down")
		const shutdownTimeout = 5 * time.Second
		shutdownCtx, cancel := context.WithTimeout(context.Background(), shutdownTimeout)
		defer cancel()

		return httpServer.Shutdown(shutdownCtx)
	case err := <-errChan:
		return err
	}
}

// registerDeviceRoutes wires the Device service. Action keys match the ONVIF
// Device WSDL verbatim. Every handler renders a prefixed, namespace-correct
// response via onvif_responses.go: response wrappers and their locally-declared
// bridge children (Capabilities, SystemDateAndTime, Manufacturer, Service) stay
// in the device/wsdl namespace, while children defined inside a tt: type
// (Device/Media/XAddr/…) emit in the schema (tt) namespace.
func registerDeviceRoutes(mux *http.ServeMux, srv *onvifserver.Server) {
	c := srv.GetConfig()
	d := newSOAPDispatcher(c.Username, c.Password)
	d.handle("GetSystemDateAndTime", asSystemDateAndTime(srv))
	d.handle("GetDeviceInformation", asDeviceInfo(srv))
	d.handle("GetCapabilities", asCapabilities(srv))
	d.handle("GetServices", asServices(srv))
	mux.Handle(c.BasePath+"/device_service", d)
}

// registerMediaRoutes wires the Media service. Responses are rendered with
// correct namespace depth (Profiles/VideoSources/MediaUri bridges in media/wsdl,
// their tt-typed children in schema) and GetStreamUri/GetSnapshotUri are
// registered under lowercase "Uri" — the ONVIF WSDL spelling (the library uses
// the non-conformant capital "URI").
func registerMediaRoutes(mux *http.ServeMux, srv *onvifserver.Server) {
	c := srv.GetConfig()
	d := newSOAPDispatcher(c.Username, c.Password)
	d.handle("GetProfiles", asProfiles(srv))
	d.handle("GetProfile", asProfile(srv))
	// Per-configuration queries ODM issues while setting up live video.
	// Before these existed, GetVideoSourceConfiguration(vs_main) faulted on
	// every attempt and ODM aborted before opening the RTSP connection.
	d.handle("GetVideoSourceConfiguration", asVideoSourceConfiguration(srv))
	d.handle("GetVideoSourceConfigurations", asVideoSourceConfigurations(srv))
	d.handle("GetVideoEncoderConfiguration", asVideoEncoderConfiguration(srv))
	d.handle("GetVideoEncoderConfigurations", asVideoEncoderConfigurations(srv))
	d.handle("GetCompatibleVideoEncoderConfigurations", asCompatibleVideoEncoderConfigurations(srv))
	d.handle("GetVideoEncoderConfigurationOptions", asVideoEncoderConfigurationOptions(srv))
	d.handle("GetAudioSourceConfiguration", asAudioSourceConfiguration(srv))
	d.handle("GetAudioSourceConfigurations", asAudioSourceConfigurations(srv))
	d.handle("GetMetadataConfigurations", asMetadataConfigurations(srv))
	d.handle("GetCompatibleMetadataConfigurations", asCompatibleMetadataConfigurations(srv))
	d.handle("GetVideoSources", asVideoSources(srv))
	d.handle("GetAudioSources", asAudioSources(srv))
	d.handle("GetStreamUri", streamUriHandler(srv))
	d.handle("GetSnapshotUri", snapshotUriHandler(srv))
	mux.Handle(c.BasePath+"/media_service", d)
}
