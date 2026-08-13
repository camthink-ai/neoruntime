package main

// Prefixed, ONVIF-spec-compliant response rendering.
//
// Why this exists (three problems with onvif-go v1.1.4 + Go encoding/xml):
//
//  1. Namespace depth. ONVIF's wsdl:types declares the response wrapper AND its
//     direct child (Capabilities, Profiles, MediaUri, SystemDateAndTime,
//     Manufacturer, ...) as *locally-scoped* elements — so they belong to the
//     service WSDL namespace (tds/trt). Only the children defined *inside* a
//     referenced tt:Something type (Device/Media under Capabilities; Name/
//     VideoEncoderConfiguration under Profile; Uri under MediaUri; ...) belong
//     to the schema ("tt") namespace. encoding/xml inherits one namespace
//     downward, so a struct-tag approach cannot place the wrapper child in WSDL
//     while placing its content in tt without redefining every intermediate
//     type. (An earlier attempt put the bridge element itself in tt, which
//     broke Identification/Maintenance.)
//
//  2. Prefixes. Real ONVIF devices emit prefixed elements (tt:, tds:, trt:)
//     with the declarations on the SOAP Envelope. ODM's .NET proxy is known to
//     be strict; default-namespace form (`xmlns="..."` per element) is risky.
//     encoding/xml cannot assign prefixes via struct tags.
//
//  3. Attributes. SystemDateAndTime's DateTimeType/DaylightSavings are schema
//     attributes, but the library emits them as child elements.
//
// Fix: render each operation's Body XML by hand with explicit prefixes and
// attribute placement, and wrap it in a prefixed envelope (soaphttp.go). Data
// still comes from the library's Handle* methods (single source of truth for
// XAddr host, profile config, stream-URI overrides); only the wire formatting
// is replaced.

import (
	"bytes"
	"encoding/xml"
	"fmt"
	"log"
	"strconv"
	"strings"

	onvifserver "github.com/0x524a/onvif-go/server"
	"github.com/0x524a/onvif-go/server/soap"
)

// prefixedBody is a pre-rendered SOAP <Body> inner XML (the operation response
// element, e.g. <tds:GetDeviceInformationResponse>…</…>) using explicit
// prefixes. soaphttp.go wraps it in the prefixed envelope.
type prefixedBody string

// xbuf builds XML with explicit element prefixes.
type xbuf struct{ b bytes.Buffer }

func (x *xbuf) Open(tag string) { fmt.Fprintf(&x.b, "<%s>", tag) }

// OpenAttr opens an element with unqualified attributes given as key/value pairs.
func (x *xbuf) OpenAttr(tag string, attrs ...string) {
	fmt.Fprintf(&x.b, "<%s", tag)
	for i := 0; i+1 < len(attrs); i += 2 {
		fmt.Fprintf(&x.b, " %s=\"%s\"", attrs[i], esc(attrs[i+1]))
	}
	x.b.WriteByte('>')
}

// Empty emits an empty element with unqualified attributes.
func (x *xbuf) Empty(tag string, attrs ...string) {
	fmt.Fprintf(&x.b, "<%s", tag)
	for i := 0; i+1 < len(attrs); i += 2 {
		fmt.Fprintf(&x.b, " %s=\"%s\"", attrs[i], esc(attrs[i+1]))
	}
	x.b.WriteString("/>")
}

func (x *xbuf) Close(tag string) { fmt.Fprintf(&x.b, "</%s>", tag) }

// Elem emits <tag>escaped-value</tag>.
func (x *xbuf) Elem(tag, val string) {
	fmt.Fprintf(&x.b, "<%s>%s</%s>", tag, esc(val), tag)
}

func (x *xbuf) String() string { return x.b.String() }

// esc escapes XML text/attribute values.
func esc(s string) string {
	r := strings.NewReplacer("&", "&amp;", "<", "&lt;", ">", "&gt;", "\"", "&quot;", "'", "&apos;")
	return r.Replace(s)
}

func btoa(v bool) string { return strconv.FormatBool(v) }
func itoa(v int) string  { return strconv.Itoa(v) }
func ftoa(v float64) string { return strconv.FormatFloat(v, 'f', -1, 64) }

// --- Device service renderers ---

func renderGetDeviceInformation(d onvifserver.DeviceInfo) prefixedBody {
	var x xbuf
	x.Open("tds:GetDeviceInformationResponse")
	x.Elem("tds:Manufacturer", d.Manufacturer)
	x.Elem("tds:Model", d.Model)
	x.Elem("tds:FirmwareVersion", d.FirmwareVersion)
	x.Elem("tds:SerialNumber", d.SerialNumber)
	x.Elem("tds:HardwareId", d.HardwareID)
	x.Close("tds:GetDeviceInformationResponse")
	return prefixedBody(x.String())
}

func renderGetServices(services []onvifserver.Service) prefixedBody {
	var x xbuf
	x.Open("tds:GetServicesResponse")
	for _, s := range services {
		x.Open("tds:Service")
		x.Elem("tds:Namespace", s.Namespace)
		x.Elem("tds:XAddr", s.XAddr)
		x.Open("tds:Version")
		x.Elem("tds:Major", itoa(s.Version.Major))
		x.Elem("tds:Minor", itoa(s.Version.Minor))
		x.Close("tds:Version")
		x.Close("tds:Service")
	}
	x.Close("tds:GetServicesResponse")
	return prefixedBody(x.String())
}

func renderGetCapabilities(c *onvifserver.Capabilities) prefixedBody {
	var x xbuf
	x.Open("tds:GetCapabilitiesResponse")
	x.Open("tds:Capabilities")
	if c.Analytics != nil {
		x.Empty("tt:Analytics", "XAddr", c.Analytics.XAddr)
	}
	if c.Device != nil {
		d := c.Device
		x.Open("tt:Device")
		x.Elem("tt:XAddr", d.XAddr)
		if d.Network != nil {
			n := d.Network
			x.Empty("tt:Network", "IPFilter", btoa(n.IPFilter), "ZeroConfiguration", btoa(n.ZeroConfiguration),
				"IPVersion6", btoa(n.IPVersion6), "DynDNS", btoa(n.DynDNS))
		}
		if d.System != nil {
			s := d.System
			x.Empty("tt:System", "DiscoveryResolve", btoa(s.DiscoveryResolve), "DiscoveryBye", btoa(s.DiscoveryBye),
				"RemoteDiscovery", btoa(s.RemoteDiscovery), "SystemBackup", btoa(s.SystemBackup),
				"SystemLogging", btoa(s.SystemLogging), "FirmwareUpgrade", btoa(s.FirmwareUpgrade))
		}
		if d.IO != nil {
			x.Empty("tt:IO", "InputConnectors", itoa(d.IO.InputConnectors), "RelayOutputs", itoa(d.IO.RelayOutputs))
		}
		if d.Security != nil {
			s := d.Security
			x.Empty("tt:Security", "TLS1.1", btoa(s.TLS11), "TLS1.2", btoa(s.TLS12),
				"OnboardKeyGeneration", btoa(s.OnboardKeyGeneration), "AccessPolicyConfig", btoa(s.AccessPolicyConfig),
				"X.509Token", btoa(s.X509Token), "SAMLToken", btoa(s.SAMLToken),
				"KerberosToken", btoa(s.KerberosToken), "RELToken", btoa(s.RELToken))
		}
		x.Close("tt:Device")
	}
	if c.Events != nil {
		e := c.Events
		x.Empty("tt:Events", "XAddr", e.XAddr,
			"WSSubscriptionPolicySupport", btoa(e.WSSubscriptionPolicySupport),
			"WSPullPointSupport", btoa(e.WSPullPointSupport),
			"WSPausableSubscriptionManagerInterfaceSupport", btoa(e.WSPausableSubscriptionSupport))
	}
	if c.Imaging != nil {
		x.Empty("tt:Imaging", "XAddr", c.Imaging.XAddr)
	}
	if c.Media != nil {
		m := c.Media
		x.Open("tt:Media")
		x.Elem("tt:XAddr", m.XAddr)
		if m.StreamingCapabilities != nil {
			sc := m.StreamingCapabilities
			x.Empty("tt:StreamingCapabilities", "RTPMulticast", btoa(sc.RTPMulticast),
				"RTP_TCP", btoa(sc.RTPTCP), "RTP_RTSP_TCP", btoa(sc.RTPRTSPTCP))
		}
		x.Close("tt:Media")
	}
	if c.PTZ != nil {
		x.Empty("tt:PTZ", "XAddr", c.PTZ.XAddr)
	}
	x.Close("tds:Capabilities")
	x.Close("tds:GetCapabilitiesResponse")
	return prefixedBody(x.String())
}

// renderGetSystemDateAndTime emits DateTimeType/DaylightSavings as attributes
// (the library wrongly emits them as elements) and the time subtree in tt.
func renderGetSystemDateAndTime(sd soap.SystemDateAndTime) prefixedBody {
	var x xbuf
	x.Open("tds:GetSystemDateAndTimeResponse")
	x.OpenAttr("tds:SystemDateAndTime", "DateTimeType", sd.DateTimeType, "DaylightSavings", btoa(sd.DaylightSavings))
	if sd.TimeZone.TZ != "" {
		x.Open("tt:TimeZone")
		x.Elem("tt:TZ", sd.TimeZone.TZ)
		x.Close("tt:TimeZone")
	}
	renderDateTime(&x, "tt:UTCDateTime", sd.UTCDateTime)
	renderDateTime(&x, "tt:LocalDateTime", sd.LocalDateTime)
	x.Close("tds:SystemDateAndTime")
	x.Close("tds:GetSystemDateAndTimeResponse")
	return prefixedBody(x.String())
}

func renderDateTime(x *xbuf, tag string, dt soap.DateTime) {
	x.Open(tag)
	x.Open("tt:Time")
	x.Elem("tt:Hour", itoa(dt.Time.Hour))
	x.Elem("tt:Minute", itoa(dt.Time.Minute))
	x.Elem("tt:Second", itoa(dt.Time.Second))
	x.Close("tt:Time")
	x.Open("tt:Date")
	x.Elem("tt:Year", itoa(dt.Date.Year))
	x.Elem("tt:Month", itoa(dt.Date.Month))
	x.Elem("tt:Day", itoa(dt.Date.Day))
	x.Close("tt:Date")
	x.Close(tag)
}

// --- Media service renderers ---

// renderProfileElement emits a single <trt:TAG token=… fixed=…>…tt children…</trt:TAG>.
// Used by GetProfiles (tag "Profiles", many) and GetProfile (tag "Profile", one).
// The element is locally-declared in media/wsdl (trt); its tt-typed children emit
// in the schema namespace.
func renderProfileElement(x *xbuf, tag string, p onvifserver.MediaProfile) {
	x.OpenAttr(tag, "token", p.Token, "fixed", btoa(p.Fixed))
	x.Elem("tt:Name", p.Name)
	if p.VideoSourceConfiguration != nil {
		writeVideoSourceConfiguration(x, p.VideoSourceConfiguration)
	}
	if p.VideoEncoderConfiguration != nil {
		writeVideoEncoderConfiguration(x, p.VideoEncoderConfiguration)
	}
	if p.AudioSourceConfiguration != nil {
		writeAudioSourceConfiguration(x, p.AudioSourceConfiguration)
	}
	if p.PTZConfiguration != nil {
		pz := p.PTZConfiguration
		x.OpenAttr("tt:PTZConfiguration", "token", pz.Token)
		x.Elem("tt:Name", pz.Name)
		x.Elem("tt:UseCount", itoa(pz.UseCount))
		x.Elem("tt:NodeToken", pz.NodeToken)
		x.Close("tt:PTZConfiguration")
	}
	x.Close(tag)
}

// --- Configuration subtree writers (shared by GetProfiles/GetProfile and the
// singular/plural Get*Configuration responses so the wire form never drifts). ---

func writeVideoSourceConfiguration(x *xbuf, v *onvifserver.VideoSourceConfiguration) {
	x.OpenAttr("tt:VideoSourceConfiguration", "token", v.Token, "fixed", "false")
	x.Elem("tt:Name", v.Name)
	x.Elem("tt:UseCount", itoa(v.UseCount))
	x.Elem("tt:SourceToken", v.SourceToken)
	x.Empty("tt:Bounds", "x", itoa(v.Bounds.X), "y", itoa(v.Bounds.Y),
		"width", itoa(v.Bounds.Width), "height", itoa(v.Bounds.Height))
	x.Close("tt:VideoSourceConfiguration")
}

func writeVideoEncoderConfiguration(x *xbuf, e *onvifserver.VideoEncoderConfiguration) {
	// The ONVIF Configuration base type marks fixed as use="required"; a strict
	// client (ODM's .NET proxy) cannot deserialize a config that omits it and
	// renders the editor read-only. fixed="false" advertises that this config is
	// modifiable — which is true, we honour SetVideoEncoderConfiguration.
	x.OpenAttr("tt:VideoEncoderConfiguration", "token", e.Token, "fixed", "false")
	x.Elem("tt:Name", e.Name)
	x.Elem("tt:UseCount", itoa(e.UseCount))
	x.Elem("tt:Encoding", e.Encoding)
	x.Open("tt:Resolution")
	x.Elem("tt:Width", itoa(e.Resolution.Width))
	x.Elem("tt:Height", itoa(e.Resolution.Height))
	x.Close("tt:Resolution")
	x.Elem("tt:Quality", ftoa(e.Quality))
	if e.RateControl != nil {
		r := e.RateControl
		x.Open("tt:RateControl")
		x.Elem("tt:FrameRateLimit", itoa(r.FrameRateLimit))
		x.Elem("tt:EncodingInterval", itoa(r.EncodingInterval))
		x.Elem("tt:BitrateLimit", itoa(r.BitrateLimit))
		x.Close("tt:RateControl")
	}
	if e.H264 != nil {
		x.Open("tt:H264")
		x.Elem("tt:GovLength", itoa(e.H264.GovLength))
		x.Elem("tt:H264Profile", e.H264.H264Profile)
		x.Close("tt:H264")
	}
	// Multicast is REQUIRED by the ver10 VideoEncoderConfiguration sequence
	// (no minOccurs=0); it sits between the codec block and SessionTimeout. We
	// do not multicast-stream, so advertise the inert no-multicast form real
	// cameras use (0.0.0.0:0, TTL 0, AutoStart=false). Omitting it leaves the
	// element null in a strict client's proxy — ODM's Video streaming panel
	// then dereferences it and throws "未将对象引用到对象的实例" (NullRef).
	x.Open("tt:Multicast")
	x.Open("tt:Address")
	x.Elem("tt:Type", "IPv4")
	x.Elem("tt:IPv4Address", "0.0.0.0")
	x.Close("tt:Address")
	x.Elem("tt:Port", "0")
	x.Elem("tt:TTL", "0")
	x.Elem("tt:AutoStart", "false")
	x.Close("tt:Multicast")
	x.Elem("tt:SessionTimeout", e.SessionTimeout)
	x.Close("tt:VideoEncoderConfiguration")
}

func writeAudioSourceConfiguration(x *xbuf, a *onvifserver.AudioSourceConfiguration) {
	x.OpenAttr("tt:AudioSourceConfiguration", "token", a.Token, "fixed", "false")
	x.Elem("tt:Name", a.Name)
	x.Elem("tt:UseCount", itoa(a.UseCount))
	x.Elem("tt:SourceToken", a.SourceToken)
	x.Close("tt:AudioSourceConfiguration")
}

func renderGetProfiles(profiles []onvifserver.MediaProfile) prefixedBody {
	var x xbuf
	x.Open("trt:GetProfilesResponse")
	for _, p := range profiles {
		renderProfileElement(&x, "trt:Profiles", p)
	}
	x.Close("trt:GetProfilesResponse")
	return prefixedBody(x.String())
}

// renderGetProfile emits a single-profile response for trt:GetProfile (the library
// ships only GetProfiles plural).
func renderGetProfile(p onvifserver.MediaProfile) prefixedBody {
	var x xbuf
	x.Open("trt:GetProfileResponse")
	renderProfileElement(&x, "trt:Profile", p)
	x.Close("trt:GetProfileResponse")
	return prefixedBody(x.String())
}

// loadProfiles fetches the canonical profile list from the library (single
// source of truth for all configuration data). The singular/plural configuration
// handlers filter this list rather than re-deriving data themselves.
func loadProfiles(srv *onvifserver.Server) ([]onvifserver.MediaProfile, error) {
	resp, err := srv.HandleGetProfiles(nil)
	if err != nil {
		return nil, err
	}
	lib, ok := resp.(*onvifserver.GetProfilesResponse)
	if !ok {
		return nil, fmt.Errorf("unexpected GetProfiles response type %T", resp)
	}
	enrichProfiles(lib.Profiles)
	return lib.Profiles, nil
}

// enrichProfiles overlays live (runtime) encoder parameters from camera-daemon
// onto each profile, then fixes H264Profile. onvif.yaml is a static declaration
// and drifts from reality because camera-daemon's codec/resolution/bitrate/fps/
// gop are runtime-mutable (web UI, ReconfigurePipeline, SwitchProfile). When the
// package-wide liveStreams provider is armed (initLiveStreams at startup) and
// camera-daemon reports an active, hardware-backed stream for a profile's token,
// the live values replace the static ones; otherwise the static values stand.
//
// H264Profile is then forced to "High": the onvif-go library hardcodes "Main"
// (server/media.go) and ignores config, while the Hailo encoder emits High for
// both streams (live SDP profile-level-id: main High@L5.1, sub High@L3.1).
// stream status has no profile field, so this stays a deterministic override.
//
// Called on every path that renders profiles: GetProfiles/GetProfile (via
// asProfiles/asProfile) and the configuration ops (via loadProfiles), so all
// responses agree.
func enrichProfiles(profiles []onvifserver.MediaProfile) {
	var live map[string]*streamParams
	if liveStreams != nil {
		live = liveStreams.RunningParams()
	}
	for i := range profiles {
		if sp, ok := live[profiles[i].Token]; ok {
			applyLiveParams(&profiles[i], sp)
		}
		if enc := profiles[i].VideoEncoderConfiguration; enc != nil && enc.H264 != nil {
			enc.H264.H264Profile = "High"
		}
	}
}

// applyLiveParams projects one running stream's parameters onto its ONVIF
// MediaProfile's encoder/source configuration. Every field is nil-guarded and
// zero-guarded so a missing live value leaves the static field untouched.
func applyLiveParams(p *onvifserver.MediaProfile, sp *streamParams) {
	if sp == nil {
		return
	}
	if enc := p.VideoEncoderConfiguration; enc != nil {
		if sp.Codec != "" {
			enc.Encoding = strings.ToUpper(sp.Codec) // camera-daemon "h264"/"h265" → ONVIF "H264"/"H265"
		}
		if sp.Width != 0 && sp.Height != 0 {
			enc.Resolution.Width = int(sp.Width)
			enc.Resolution.Height = int(sp.Height)
		}
		if sp.Fps != 0 || sp.BitrateKbps != 0 {
			if enc.RateControl == nil {
				enc.RateControl = &onvifserver.VideoRateControl{EncodingInterval: 1}
			}
			if sp.Fps != 0 {
				enc.RateControl.FrameRateLimit = int(sp.Fps)
			}
			if sp.BitrateKbps != 0 {
				enc.RateControl.BitrateLimit = int(sp.BitrateKbps)
			}
		}
		if enc.H264 != nil && sp.Gop != 0 {
			enc.H264.GovLength = int(sp.Gop)
		}
	}
	if vsc := p.VideoSourceConfiguration; vsc != nil && sp.Width != 0 && sp.Height != 0 {
		vsc.Bounds.Width = int(sp.Width)
		vsc.Bounds.Height = int(sp.Height)
	}
}

// parseConfigToken extracts <…:ConfigurationToken>value</…> from a request body,
// namespace-tolerant (see parseProfileToken). Used by the singular Get*Configuration ops.
func parseConfigToken(body interface{}) string {
	raw, ok := body.([]byte)
	if !ok {
		return ""
	}
	dec := xml.NewDecoder(bytes.NewReader(raw))
	for {
		tok, err := dec.Token()
		if err != nil {
			return ""
		}
		if se, ok := tok.(xml.StartElement); ok && se.Name.Local == "ConfigurationToken" {
			var s string
			if err := dec.DecodeElement(&s, &se); err != nil {
				return ""
			}
			return strings.TrimSpace(s)
		}
	}
}

// --- singular/plural configuration renderers ---

func renderGetVideoSourceConfiguration(v *onvifserver.VideoSourceConfiguration) prefixedBody {
	var x xbuf
	x.Open("trt:GetVideoSourceConfigurationResponse")
	writeVideoSourceConfiguration(&x, v)
	x.Close("trt:GetVideoSourceConfigurationResponse")
	return prefixedBody(x.String())
}

func renderGetVideoSourceConfigurations(list []*onvifserver.VideoSourceConfiguration) prefixedBody {
	var x xbuf
	x.Open("trt:GetVideoSourceConfigurationsResponse")
	for _, v := range list {
		writeVideoSourceConfiguration(&x, v)
	}
	x.Close("trt:GetVideoSourceConfigurationsResponse")
	return prefixedBody(x.String())
}

func renderGetVideoEncoderConfiguration(e *onvifserver.VideoEncoderConfiguration) prefixedBody {
	var x xbuf
	x.Open("trt:GetVideoEncoderConfigurationResponse")
	writeVideoEncoderConfiguration(&x, e)
	x.Close("trt:GetVideoEncoderConfigurationResponse")
	return prefixedBody(x.String())
}

func renderGetVideoEncoderConfigurations(list []*onvifserver.VideoEncoderConfiguration) prefixedBody {
	var x xbuf
	x.Open("trt:GetVideoEncoderConfigurationsResponse")
	for _, e := range list {
		writeVideoEncoderConfiguration(&x, e)
	}
	x.Close("trt:GetVideoEncoderConfigurationsResponse")
	return prefixedBody(x.String())
}

// renderGetVideoEncoderConfigurationOptions advertises the valid encoder-option
// ranges (resolutions, framerate, GOV length, H264 profiles). Resolutions and
// the framerate ceiling are derived from the configured profiles; the rest are
// sensible defaults that match what camera-daemon actually encodes.
func renderGetVideoEncoderConfigurationOptions(profiles []onvifserver.MediaProfile) prefixedBody {
	var x xbuf
	x.Open("trt:GetVideoEncoderConfigurationOptionsResponse")
	// Per the ver10 schema, VideoEncoderConfigurationOptions' sequence is QualityRange
	// (required, first child), then the encoding-specific element (H264 here).
	// There is NO <Encoding> child — the encoding is implied by which option block is
	// present. Emitting <tt:Encoding> before QualityRange is out-of-sequence and breaks
	// strict .NET deserializers (ONVIF Device Manager then renders the config
	// read-only), so it is deliberately omitted here. Real cameras omit it too.
	//
	// WIRE FORMAT (verified against a real ODM-compatible camera — Bosch FLEXIDOME
	// indoor 5100i — and the onvif-go client struct): use the element names, NOT the
	// type names:
	//   - <trt:Options>  (locally declared in media.wsdl → trt namespace, NOT tt)
	//   - <tt:H264>      (element name is "H264"; "H264Options" is the TYPE name)
	// Emitting <tt:Options> or <tt:H264Options> makes ODM's .NET deserializer fail to
	// match the element → Options null → NullReferenceException ("未将对象引用…") when
	// the Video streaming panel renders the encoder editor. The earlier "H264Options"
	// form was a schema-reading mistake; real cameras and the onvif-go client both use
	// the element name "H264".
	x.Open("trt:Options")
	x.Open("tt:QualityRange")
	x.Elem("tt:Min", "1")
	x.Elem("tt:Max", "100")
	x.Close("tt:QualityRange")
	x.Open("tt:H264")
	seen := make(map[string]bool)
	maxFR := 30
	for _, p := range profiles {
		if p.VideoEncoderConfiguration == nil {
			continue
		}
		w := p.VideoEncoderConfiguration.Resolution.Width
		h := p.VideoEncoderConfiguration.Resolution.Height
		key := strconv.Itoa(w) + "x" + strconv.Itoa(h)
		if !seen[key] {
			seen[key] = true
			x.Open("tt:ResolutionsAvailable")
			x.Elem("tt:Width", itoa(w))
			x.Elem("tt:Height", itoa(h))
			x.Close("tt:ResolutionsAvailable")
		}
		if p.VideoEncoderConfiguration.RateControl != nil &&
			p.VideoEncoderConfiguration.RateControl.FrameRateLimit > maxFR {
			maxFR = p.VideoEncoderConfiguration.RateControl.FrameRateLimit
		}
	}
	x.Open("tt:GovLengthRange")
	x.Elem("tt:Min", "1")
	x.Elem("tt:Max", "120")
	x.Close("tt:GovLengthRange")
	x.Open("tt:FrameRateRange")
	x.Elem("tt:Min", "1")
	x.Elem("tt:Max", itoa(maxFR))
	x.Close("tt:FrameRateRange")
	x.Open("tt:EncodingIntervalRange")
	x.Elem("tt:Min", "1")
	x.Elem("tt:Max", "1")
	x.Close("tt:EncodingIntervalRange")
	for _, profile := range []string{"Baseline", "Main", "High"} {
		x.Elem("tt:H264ProfilesSupported", profile)
	}
	x.Close("tt:H264")
	x.Close("trt:Options")
	x.Close("trt:GetVideoEncoderConfigurationOptionsResponse")
	return prefixedBody(x.String())
}

func renderGetAudioSourceConfigurations(list []*onvifserver.AudioSourceConfiguration) prefixedBody {
	var x xbuf
	x.Open("trt:GetAudioSourceConfigurationsResponse")
	for _, a := range list {
		writeAudioSourceConfiguration(&x, a)
	}
	x.Close("trt:GetAudioSourceConfigurationsResponse")
	return prefixedBody(x.String())
}

func renderGetAudioSourceConfiguration(a *onvifserver.AudioSourceConfiguration) prefixedBody {
	var x xbuf
	x.Open("trt:GetAudioSourceConfigurationResponse")
	writeAudioSourceConfiguration(&x, a)
	x.Close("trt:GetAudioSourceConfigurationResponse")
	return prefixedBody(x.String())
}

// renderEmptyConfigurations emits an empty configuration list for capabilities
// we do not expose (metadata). Returning an empty (but valid) response instead
// of a fault lets clients enumerate cleanly.
func renderEmptyConfigurations(wrapper string) prefixedBody {
	var x xbuf
	x.Open(wrapper)
	x.Close(wrapper)
	return prefixedBody(x.String())
}

// renderGetAudioSources emits an empty audio-source list (Phase 1 has no audio).
func renderGetAudioSources() prefixedBody {
	var x xbuf
	x.Open("trt:GetAudioSourcesResponse")
	x.Close("trt:GetAudioSourcesResponse")
	return prefixedBody(x.String())
}

// parseProfileToken extracts <…:ProfileToken>value</…:ProfileToken> from a request
// body, matching by local name so it works regardless of the client's prefix/
// namespace — the raw Body inner-XML carries prefixes whose declarations live on
// the Envelope, so namespace-strict parsing would fail.
func parseProfileToken(body interface{}) string {
	raw, ok := body.([]byte)
	if !ok {
		return ""
	}
	dec := xml.NewDecoder(bytes.NewReader(raw))
	for {
		tok, err := dec.Token()
		if err != nil {
			return ""
		}
		if se, ok := tok.(xml.StartElement); ok && se.Name.Local == "ProfileToken" {
			var s string
			if err := dec.DecodeElement(&s, &se); err != nil {
				return ""
			}
			return strings.TrimSpace(s)
		}
	}
}

func renderGetVideoSources(sources []onvifserver.VideoSource) prefixedBody {
	var x xbuf
	x.Open("trt:GetVideoSourcesResponse")
	for _, s := range sources {
		x.OpenAttr("trt:VideoSources", "token", s.Token)
		x.Elem("tt:Framerate", ftoa(s.Framerate))
		x.Open("tt:Resolution")
		x.Elem("tt:Width", itoa(s.Resolution.Width))
		x.Elem("tt:Height", itoa(s.Resolution.Height))
		x.Close("tt:Resolution")
		x.Close("trt:VideoSources")
	}
	x.Close("trt:GetVideoSourcesResponse")
	return prefixedBody(x.String())
}

// renderGetMediaUri renders the common MediaUri payload for GetStreamUri /
// GetSnapshotUri under the given wrapper element name.
func renderGetMediaUri(wrapper string, m onvifserver.MediaURI) prefixedBody {
	var x xbuf
	x.Open(wrapper)
	x.Open("trt:MediaUri")
	x.Elem("tt:Uri", m.URI)
	x.Elem("tt:InvalidAfterConnect", btoa(m.InvalidAfterConnect))
	x.Elem("tt:InvalidAfterReboot", btoa(m.InvalidAfterReboot))
	x.Elem("tt:Timeout", m.Timeout)
	x.Close("trt:MediaUri")
	x.Close(wrapper)
	return prefixedBody(x.String())
}

// --- Adapter handlers: delegate data to the library, render prefixed XML. ---

// asDeviceInfo calls the library then renders GetDeviceInformation.
func asDeviceInfo(srv *onvifserver.Server) soapHandlerFunc {
	return func(body interface{}) (interface{}, error) {
		resp, err := srv.HandleGetDeviceInformation(body)
		if err != nil {
			return nil, err
		}
		lib, ok := resp.(*onvifserver.GetDeviceInformationResponse)
		if !ok {
			return resp, nil
		}
		return renderGetDeviceInformation(onvifserver.DeviceInfo{
			Manufacturer:    lib.Manufacturer,
			Model:           lib.Model,
			FirmwareVersion: lib.FirmwareVersion,
			SerialNumber:    lib.SerialNumber,
			HardwareID:      lib.HardwareID,
		}), nil
	}
}

func asServices(srv *onvifserver.Server) soapHandlerFunc {
	return func(body interface{}) (interface{}, error) {
		resp, err := srv.HandleGetServices(body)
		if err != nil {
			return nil, err
		}
		lib, ok := resp.(*onvifserver.GetServicesResponse)
		if !ok {
			return resp, nil
		}
		return renderGetServices(lib.Service), nil
	}
}

func asCapabilities(srv *onvifserver.Server) soapHandlerFunc {
	return func(body interface{}) (interface{}, error) {
		resp, err := srv.HandleGetCapabilities(body)
		if err != nil {
			return nil, err
		}
		lib, ok := resp.(*onvifserver.GetCapabilitiesResponse)
		if !ok || lib.Capabilities == nil {
			return resp, nil
		}
		return renderGetCapabilities(lib.Capabilities), nil
	}
}

func asSystemDateAndTime(srv *onvifserver.Server) soapHandlerFunc {
	return func(body interface{}) (interface{}, error) {
		resp, err := srv.HandleGetSystemDateAndTime(body)
		if err != nil {
			return nil, err
		}
		lib, ok := resp.(*soap.GetSystemDateAndTimeResponse)
		if !ok {
			return resp, nil
		}
		return renderGetSystemDateAndTime(lib.SystemDateAndTime), nil
	}
}

func asProfiles(srv *onvifserver.Server) soapHandlerFunc {
	return func(body interface{}) (interface{}, error) {
		resp, err := srv.HandleGetProfiles(body)
		if err != nil {
			return nil, err
		}
		lib, ok := resp.(*onvifserver.GetProfilesResponse)
		if !ok {
			return resp, nil
		}
		enrichProfiles(lib.Profiles)
		return renderGetProfiles(lib.Profiles), nil
	}
}

// asProfile handles trt:GetProfile (singular). The library only ships GetProfiles
// plural, so we reuse it and select the requested token. ODM calls GetProfile to
// fetch the full profile when starting live video.
func asProfile(srv *onvifserver.Server) soapHandlerFunc {
	return func(body interface{}) (interface{}, error) {
		token := parseProfileToken(body)
		resp, err := srv.HandleGetProfiles(body)
		if err != nil {
			return nil, err
		}
		lib, ok := resp.(*onvifserver.GetProfilesResponse)
		if !ok {
			return resp, nil
		}
		enrichProfiles(lib.Profiles)
		for _, p := range lib.Profiles {
			if p.Token == token {
				return renderGetProfile(p), nil
			}
		}
		return nil, fmt.Errorf("profile not found: %s", token)
	}
}

// renderGetCompatibleVideoEncoderConfigurations uses the spec wrapper name for
// the "Compatible*" variant; the Configurations payload is identical to the
// plain GetVideoEncoderConfigurations response.
func renderGetCompatibleVideoEncoderConfigurations(list []*onvifserver.VideoEncoderConfiguration) prefixedBody {
	var x xbuf
	x.Open("trt:GetCompatibleVideoEncoderConfigurationsResponse")
	for _, e := range list {
		writeVideoEncoderConfiguration(&x, e)
	}
	x.Close("trt:GetCompatibleVideoEncoderConfigurationsResponse")
	return prefixedBody(x.String())
}

// --- Configuration adapter handlers ---
//
// Each loads the canonical profile list via loadProfiles (single source of
// truth), then filters by ConfigurationToken (singular) or collects all
// (plural/compatible). ODM calls these per-profile while setting up live video;
// before they existed, GetVideoSourceConfiguration(vs_main) faulted 8x and ODM
// aborted before reaching RTSP.

func asVideoSourceConfiguration(srv *onvifserver.Server) soapHandlerFunc {
	return func(body interface{}) (interface{}, error) {
		token := parseConfigToken(body)
		profiles, err := loadProfiles(srv)
		if err != nil {
			return nil, err
		}
		for _, p := range profiles {
			if p.VideoSourceConfiguration != nil && p.VideoSourceConfiguration.Token == token {
				return renderGetVideoSourceConfiguration(p.VideoSourceConfiguration), nil
			}
		}
		return nil, fmt.Errorf("video source configuration not found: %s", token)
	}
}

func asVideoSourceConfigurations(srv *onvifserver.Server) soapHandlerFunc {
	return func(body interface{}) (interface{}, error) {
		profiles, err := loadProfiles(srv)
		if err != nil {
			return nil, err
		}
		seen := make(map[string]bool)
		var list []*onvifserver.VideoSourceConfiguration
		for i := range profiles {
			v := profiles[i].VideoSourceConfiguration
			if v != nil && !seen[v.Token] {
				seen[v.Token] = true
				list = append(list, v)
			}
		}
		return renderGetVideoSourceConfigurations(list), nil
	}
}

func asVideoEncoderConfiguration(srv *onvifserver.Server) soapHandlerFunc {
	return func(body interface{}) (interface{}, error) {
		token := parseConfigToken(body)
		profiles, err := loadProfiles(srv)
		if err != nil {
			return nil, err
		}
		for _, p := range profiles {
			if p.VideoEncoderConfiguration != nil && p.VideoEncoderConfiguration.Token == token {
				return renderGetVideoEncoderConfiguration(p.VideoEncoderConfiguration), nil
			}
		}
		return nil, fmt.Errorf("video encoder configuration not found: %s", token)
	}
}

// --- SetVideoEncoderConfiguration (write path) ---
//
// onvif-device is otherwise read-only; this is the first Set operation. An NVR
// sends the full VideoEncoderConfiguration it previously read via Get (with its
// own edits) plus ForcePersistence. We project the mutable encoder fields onto
// camera-daemon's ReconfigureEncoder, where 0/empty means "no change" — so a
// client that only edits the bitrate does not disturb the resolution. Because
// enrichProfiles re-reads GetStreamStatus on every request, the next Get
// reflects the new params immediately (closed loop). ForcePersistence is
// accepted but not honoured separately: persistence is camera-daemon's
// responsibility (it owns camera-daemon.yaml); we apply the live change
// regardless of the flag, matching how the device web UI applies changes.

// onvifSetVideoEncoder parses the incoming SetVideoEncoderConfiguration body.
// Only the mutable encoder fields are captured; Name/UseCount/Quality/
// SessionTimeout are accepted and ignored. Local names match regardless of
// namespace prefix, so both tt:-prefixed and default-namespaced bodies parse.
type onvifSetVideoEncoder struct {
	XMLName xml.Name `xml:"SetVideoEncoderConfiguration"`
	Config  struct {
		Token      string `xml:"token,attr"`
		Encoding   string `xml:"Encoding"`
		Resolution struct {
			Width  int `xml:"Width"`
			Height int `xml:"Height"`
		} `xml:"Resolution"`
		RateControl struct {
			FrameRateLimit int `xml:"FrameRateLimit"`
			BitrateLimit   int `xml:"BitrateLimit"`
		} `xml:"RateControl"`
		H264 struct {
			GovLength int `xml:"GovLength"`
		} `xml:"H264"`
	} `xml:"Configuration"`
}

// streamNameFromEncoderToken maps an encoder configuration token (e.g.
// "main_encoder", as the library assigns Token+"_encoder") back to the
// camera-daemon stream name ("main"). Returns "" if the token is not in that
// form. The stream name equals the profile token in this codebase (onvif.yaml
// stream == token by default), and camera-daemon's RTSP mounts and
// ReconfigureEncoder stream_name both use it.
func streamNameFromEncoderToken(token string) string {
	if s := strings.TrimSuffix(token, "_encoder"); s != token && s != "" {
		return s
	}
	return ""
}

// asSetVideoEncoderConfiguration applies an NVR-supplied encoder configuration
// to the matching camera-daemon stream. It validates the token, maps it to a
// stream, and calls ReconfigureEncoder; any failure becomes a SOAP fault.
func asSetVideoEncoderConfiguration(srv *onvifserver.Server) soapHandlerFunc {
	return func(body interface{}) (interface{}, error) {
		raw, ok := body.([]byte)
		if !ok {
			return nil, fmt.Errorf("empty SetVideoEncoderConfiguration body")
		}
		var req onvifSetVideoEncoder
		if err := xml.Unmarshal(raw, &req); err != nil {
			return nil, fmt.Errorf("parse SetVideoEncoderConfiguration: %w", err)
		}
		token := req.Config.Token

		// Validate the token refers to a known encoder config (clean "not found"
		// fault without depending on camera-daemon being up).
		profiles, err := loadProfiles(srv)
		if err != nil {
			return nil, err
		}
		known := false
		for _, p := range profiles {
			if p.VideoEncoderConfiguration != nil && p.VideoEncoderConfiguration.Token == token {
				known = true
				break
			}
		}
		if !known {
			return nil, fmt.Errorf("video encoder configuration not found: %s", token)
		}
		stream := streamNameFromEncoderToken(token)
		if stream == "" {
			return nil, fmt.Errorf("cannot map encoder token %q to a camera-daemon stream", token)
		}
		if reconfigurer == nil {
			return nil, fmt.Errorf("camera-daemon not available; encoder configuration cannot be changed via ONVIF")
		}

		params := streamParams{
			Codec:       req.Config.Encoding,
			Width:       uint32(req.Config.Resolution.Width),
			Height:      uint32(req.Config.Resolution.Height),
			Fps:         uint32(req.Config.RateControl.FrameRateLimit),
			BitrateKbps: uint32(req.Config.RateControl.BitrateLimit),
			Gop:         uint32(req.Config.H264.GovLength),
		}
		log.Printf("[onvif] SetVideoEncoderConfiguration token=%s -> stream=%s codec=%s %dx%d fps=%d bitrate=%dkbps gop=%d",
			token, stream, params.Codec, params.Width, params.Height, params.Fps, params.BitrateKbps, params.Gop)
		if err := reconfigurer.ReconfigureEncoder(stream, params); err != nil {
			log.Printf("[onvif] ReconfigureEncoder(%s) FAILED: %v", stream, err)
			return nil, fmt.Errorf("ReconfigureEncoder(%s): %w", stream, err)
		}
		log.Printf("[onvif] ReconfigureEncoder(%s) accepted", stream)
		return prefixedBody("<trt:SetVideoEncoderConfigurationResponse/>"), nil
	}
}

func asVideoEncoderConfigurations(srv *onvifserver.Server) soapHandlerFunc {
	return func(body interface{}) (interface{}, error) {
		profiles, err := loadProfiles(srv)
		if err != nil {
			return nil, err
		}
		seen := make(map[string]bool)
		var list []*onvifserver.VideoEncoderConfiguration
		for i := range profiles {
			e := profiles[i].VideoEncoderConfiguration
			if e != nil && !seen[e.Token] {
				seen[e.Token] = true
				list = append(list, e)
			}
		}
		return renderGetVideoEncoderConfigurations(list), nil
	}
}

func asCompatibleVideoEncoderConfigurations(srv *onvifserver.Server) soapHandlerFunc {
	return func(body interface{}) (interface{}, error) {
		profiles, err := loadProfiles(srv)
		if err != nil {
			return nil, err
		}
		seen := make(map[string]bool)
		var list []*onvifserver.VideoEncoderConfiguration
		for i := range profiles {
			e := profiles[i].VideoEncoderConfiguration
			if e != nil && !seen[e.Token] {
				seen[e.Token] = true
				list = append(list, e)
			}
		}
		return renderGetCompatibleVideoEncoderConfigurations(list), nil
	}
}

func asVideoEncoderConfigurationOptions(srv *onvifserver.Server) soapHandlerFunc {
	return func(body interface{}) (interface{}, error) {
		profiles, err := loadProfiles(srv)
		if err != nil {
			return nil, err
		}
		return renderGetVideoEncoderConfigurationOptions(profiles), nil
	}
}

func asAudioSourceConfiguration(srv *onvifserver.Server) soapHandlerFunc {
	return func(body interface{}) (interface{}, error) {
		token := parseConfigToken(body)
		profiles, err := loadProfiles(srv)
		if err != nil {
			return nil, err
		}
		for _, p := range profiles {
			if p.AudioSourceConfiguration != nil && p.AudioSourceConfiguration.Token == token {
				return renderGetAudioSourceConfiguration(p.AudioSourceConfiguration), nil
			}
		}
		return nil, fmt.Errorf("audio source configuration not found: %s", token)
	}
}

func asAudioSourceConfigurations(srv *onvifserver.Server) soapHandlerFunc {
	return func(body interface{}) (interface{}, error) {
		profiles, err := loadProfiles(srv)
		if err != nil {
			return nil, err
		}
		seen := make(map[string]bool)
		var list []*onvifserver.AudioSourceConfiguration
		for i := range profiles {
			a := profiles[i].AudioSourceConfiguration
			if a != nil && !seen[a.Token] {
				seen[a.Token] = true
				list = append(list, a)
			}
		}
		return renderGetAudioSourceConfigurations(list), nil
	}
}

func asMetadataConfigurations(srv *onvifserver.Server) soapHandlerFunc {
	return func(body interface{}) (interface{}, error) {
		return renderEmptyConfigurations("trt:GetMetadataConfigurationsResponse"), nil
	}
}

func asCompatibleMetadataConfigurations(srv *onvifserver.Server) soapHandlerFunc {
	return func(body interface{}) (interface{}, error) {
		return renderEmptyConfigurations("trt:GetCompatibleMetadataConfigurationsResponse"), nil
	}
}

// asAudioSources returns an empty audio-source list (Phase 1 has no audio).
func asAudioSources(srv *onvifserver.Server) soapHandlerFunc {
	return func(body interface{}) (interface{}, error) {
		return renderGetAudioSources(), nil
	}
}

func asVideoSources(srv *onvifserver.Server) soapHandlerFunc {
	return func(body interface{}) (interface{}, error) {
		resp, err := srv.HandleGetVideoSources(body)
		if err != nil {
			return nil, err
		}
		lib, ok := resp.(*onvifserver.GetVideoSourcesResponse)
		if !ok {
			return resp, nil
		}
		return renderGetVideoSources(lib.VideoSources), nil
	}
}

func streamUriHandler(srv *onvifserver.Server) soapHandlerFunc {
	return func(body interface{}) (interface{}, error) {
		resp, err := srv.HandleGetStreamURI(body)
		if err != nil {
			return nil, err
		}
		lib, ok := resp.(*onvifserver.GetStreamURIResponse)
		if !ok {
			return resp, nil
		}
		return renderGetMediaUri("trt:GetStreamUriResponse", lib.MediaURI), nil
	}
}

func snapshotUriHandler(srv *onvifserver.Server) soapHandlerFunc {
	return func(body interface{}) (interface{}, error) {
		resp, err := srv.HandleGetSnapshotURI(body)
		if err != nil {
			return nil, err
		}
		lib, ok := resp.(*onvifserver.GetSnapshotURIResponse)
		if !ok {
			return resp, nil
		}
		return renderGetMediaUri("trt:GetSnapshotUriResponse", lib.MediaURI), nil
	}
}
