package main

// Thin ONVIF SOAP-over-HTTP dispatcher.
//
// The onvif-go library ships a soap.Handler whose ServeHTTP passes
// envelope.Body.Content to handlers. Body.Content is a nil interface{}:
// encoding/xml does not populate nil interfaces, so every handler that reads
// the request body (GetStreamUri, GetSnapshotUri, all Imaging/PTZ) fails with
// "failed to unmarshal XML: EOF". The library's own tests sidestep this by
// calling Handle*([]byte(...)) directly, never over HTTP.
//
// soapDispatcher fixes that: it extracts the raw <Body> inner XML and hands it
// to handlers as []byte, exactly as the library's tests do. It also enforces
// WS-Security UsernameToken digest auth when credentials are configured, and
// renders SOAP 1.2 responses/faults. This is the "hand-written thin SOAP layer"
// fallback the implementation plan anticipated.

import (
	"bytes"
	"crypto/sha1" //nolint:gosec // SHA1 required by the ONVIF digest profile
	"encoding/base64"
	"encoding/xml"
	"io"
	"log"
	"net/http"
	"strings"
)

// soapHandlerFunc is the shape of every onvif-go Handle* method.
type soapHandlerFunc func(body interface{}) (interface{}, error)

// soapDispatcher routes ONVIF SOAP actions to handlers, passing the request
// body element as []byte.
type soapDispatcher struct {
	username string
	password string
	handlers map[string]soapHandlerFunc
}

func newSOAPDispatcher(username, password string) *soapDispatcher {
	return &soapDispatcher{
		username: username,
		password: password,
		handlers: make(map[string]soapHandlerFunc),
	}
}

func (d *soapDispatcher) handle(action string, h soapHandlerFunc) {
	d.handlers[action] = h
}

func (d *soapDispatcher) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
		return
	}

	raw, err := io.ReadAll(r.Body)
	if err != nil {
		writeSOAPFault(w, "Receiver", "Failed to read request body", err.Error())
		return
	}
	_ = r.Body.Close()

	var env struct {
		Body struct {
			Content []byte `xml:",innerxml"`
		} `xml:"Body"`
		Header struct {
			Security *wsSecurity `xml:"Security"`
		} `xml:"Header"`
	}
	if err := xml.Unmarshal(raw, &env); err != nil {
		writeSOAPFault(w, "Sender", "Invalid SOAP envelope", err.Error())
		return
	}

	action := firstElementName(env.Body.Content)
	if action == "" {
		writeSOAPFault(w, "Sender", "Unknown action", "Could not determine request action")
		return
	}

	if d.username != "" && d.password != "" {
		token := env.Header.Security
		if token == nil || token.UsernameToken == nil || !token.UsernameToken.valid(d.username, d.password) {
			writeSOAPFault(w, "Sender", "Authentication failed", "Invalid username or password")
			return
		}
	}

	h, ok := d.handlers[action]
	if !ok {
		writeSOAPFault(w, "Sender", "Action not supported", "No handler for action: "+action)
		return
	}

	resp, err := h(env.Body.Content)
	if err != nil {
		writeSOAPFault(w, "Receiver", "Handler error", err.Error())
		return
	}

	writeSOAPResponse(w, resp)
}

// firstElementName returns the local name of the first child element in a <Body>
// inner-XML fragment (namespace-agnostic, so any prefix works).
func firstElementName(inner []byte) string {
	dec := xml.NewDecoder(bytes.NewReader(inner))
	for {
		tok, err := dec.Token()
		if err != nil {
			return ""
		}
		if se, ok := tok.(xml.StartElement); ok {
			return se.Name.Local
		}
	}
}

// --- WS-Security UsernameToken digest (ONVIF profile) ---

type wsSecurity struct {
	UsernameToken *wsUsernameToken `xml:"UsernameToken"`
}

type wsUsernameToken struct {
	Username string `xml:"Username"`
	Nonce    string `xml:"Nonce"`
	Created  string `xml:"Created"`
	Password string `xml:"Password"`
}

// valid checks the ONVIF digest: base64(sha1(nonce + created + password)).
func (t *wsUsernameToken) valid(user, password string) bool {
	if t.Username != user {
		return false
	}
	nonce, err := base64.StdEncoding.DecodeString(strings.TrimSpace(t.Nonce))
	if err != nil {
		return false
	}
	h := sha1.New() //nolint:gosec // ONVIF digest profile mandates SHA-1
	h.Write(nonce)
	h.Write([]byte(t.Created))
	h.Write([]byte(password))

	return base64.StdEncoding.EncodeToString(h.Sum(nil)) == strings.TrimSpace(t.Password)
}

// --- SOAP response/fault rendering ---

type soapResponseEnvelope struct {
	XMLName xml.Name `xml:"http://www.w3.org/2003/05/soap-envelope Envelope"`
	Body    struct {
		Content interface{} `xml:",omitempty"`
	} `xml:"http://www.w3.org/2003/05/soap-envelope Body"`
}

// writeSOAPResponse renders the handler result. A prefixedBody is pre-rendered
// Body XML using explicit tt:/tds:/trt: prefixes (see onvif_responses.go) and is
// wrapped in a prefixed envelope that matches real ONVIF devices; any other type
// is marshaled via encoding/xml (used by tests and faults).
func writeSOAPResponse(w http.ResponseWriter, resp interface{}) {
	if pb, ok := resp.(prefixedBody); ok {
		writePrefixedSOAP(w, string(pb))
		return
	}
	env := soapResponseEnvelope{}
	env.Body.Content = resp
	out, err := xml.Marshal(env)
	if err != nil {
		// Marshaling our own response types should never fail; if it does, surface
		// it as a fault rather than writing a partial body.
		log.Printf("[onvif] SOAP response marshal failed: %v", err)
		writeSOAPFault(w, "Receiver", "Failed to marshal response", err.Error())
		return
	}
	w.Header().Set("Content-Type", "application/soap+xml; charset=utf-8")
	_, _ = w.Write(out)
}

// writePrefixedSOAP wraps a pre-rendered inner Body in a SOAP 1.2 envelope that
// declares the tt/tds/trt prefixes ONVIF clients expect on the Envelope. Real
// ONVIF devices emit prefixed elements; ODM's .NET proxy is strict, so default-
// namespace form is avoided on purpose.
func writePrefixedSOAP(w http.ResponseWriter, inner string) {
	env := `<?xml version="1.0" encoding="UTF-8"?>` +
		`<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"` +
		` xmlns:tds="http://www.onvif.org/ver10/device/wsdl"` +
		` xmlns:trt="http://www.onvif.org/ver10/media/wsdl"` +
		` xmlns:tt="http://www.onvif.org/ver10/schema">` +
		`<s:Body>` + inner + `</s:Body></s:Envelope>`
	w.Header().Set("Content-Type", "application/soap+xml; charset=utf-8")
	_, _ = w.Write([]byte(env))
}

func writeSOAPFault(w http.ResponseWriter, code, reason, detail string) {
	// Hand-render a SOAP 1.2 fault in the SAME prefixed envelope as success
	// responses (writePrefixedSOAP). The previous implementation marshaled via
	// encoding/xml into the DEFAULT namespace, yielding an un-prefixed envelope
	// with an unqualified <Value> (e.g. <Value>Receiver</Value>). That is
	// invalid SOAP 1.2 — Value must be a QName such as s:Receiver — and
	// inconsistent with the prefixed responses ODM parses successfully. ONVIF
	// Device Manager's strict .NET/WCF fault deserializer could not match it and
	// surfaced a NullReferenceException ("未将对象引用到对象的实例") whenever it
	// hit an unimplemented op (GetScopes/GetDNS/GetNetworkInterfaces/
	// GetSnapshotUri). Matching real ONVIF devices' prefixed form resolves it.
	var b strings.Builder
	b.WriteString(`<?xml version="1.0" encoding="UTF-8"?>`)
	b.WriteString(`<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"`)
	b.WriteString(` xmlns:tds="http://www.onvif.org/ver10/device/wsdl"`)
	b.WriteString(` xmlns:trt="http://www.onvif.org/ver10/media/wsdl"`)
	b.WriteString(` xmlns:tt="http://www.onvif.org/ver10/schema">`)
	b.WriteString(`<s:Body><s:Fault><s:Code><s:Value>s:` + code + `</s:Value></s:Code>`)
	b.WriteString(`<s:Reason><s:Text xml:lang="en">` + xmlEscape(reason) + `</s:Text></s:Reason>`)
	if detail != "" {
		b.WriteString(`<s:Detail>` + xmlEscape(detail) + `</s:Detail>`)
	}
	b.WriteString(`</s:Fault></s:Body></s:Envelope>`)
	w.Header().Set("Content-Type", "application/soap+xml; charset=utf-8")
	w.WriteHeader(http.StatusInternalServerError)
	_, _ = w.Write([]byte(b.String()))
}

// xmlEscape escapes XML meta-characters in hand-rendered fault text. The SOAP
// action echoed in fault <Detail> is client-supplied, so the output must stay
// well-formed.
func xmlEscape(s string) string {
	r := strings.NewReplacer(`&`, "&amp;", `<`, "&lt;", `>`, "&gt;", `"`, "&quot;")
	return r.Replace(s)
}
