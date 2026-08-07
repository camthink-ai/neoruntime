package factoryeeprom

import (
	"context"
	"errors"
	"fmt"
	"net"
	"os"
	"os/exec"
	"regexp"
	"strings"
	"time"
)

const (
	DefaultToolPath = "/etc/factory-eeprom.sh"

	EnvToolPath   = "AIPC_FACTORY_EEPROM_TOOL"
	EnvDevicePath = "AIPC_FACTORY_EEPROM_PATH"
)

type Field string

const (
	FieldSN    Field = "SN"
	FieldMAC   Field = "MAC"
	FieldPN    Field = "PN"
	FieldBatch Field = "BATCH"
	FieldHWRev Field = "HWREV"
)

var (
	allFields = []Field{FieldSN, FieldMAC, FieldPN, FieldBatch, FieldHWRev}

	asciiFactoryValueRE = regexp.MustCompile(`^[A-Za-z0-9._:-]+$`)
)

// Info is the platform-facing view of the factory EEPROM identity block.
type Info struct {
	Available        bool   `json:"available"`
	Source           string `json:"source,omitempty"`
	SerialNumber     string `json:"serial_number,omitempty"`
	MACAddress       string `json:"mac_address,omitempty"`
	ProductNumber    string `json:"product_number,omitempty"`
	Batch            string `json:"batch,omitempty"`
	HardwareRevision string `json:"hardware_revision,omitempty"`
	Error            string `json:"error,omitempty"`
}

type Client struct {
	ToolPath   string
	DevicePath string
	Timeout    time.Duration
}

func DefaultClient() Client {
	toolPath := strings.TrimSpace(os.Getenv(EnvToolPath))
	if toolPath == "" {
		toolPath = DefaultToolPath
	}
	return Client{
		ToolPath:   toolPath,
		DevicePath: strings.TrimSpace(os.Getenv(EnvDevicePath)),
		Timeout:    2 * time.Second,
	}
}

func (c Client) Read(ctx context.Context) (Info, error) {
	toolPath := c.toolPath()
	info := Info{
		Source: toolPath,
	}
	if _, err := os.Stat(toolPath); err != nil {
		info.Error = fmt.Sprintf("factory EEPROM tool unavailable: %v", err)
		return info, errors.New(info.Error)
	}

	var readErrs []string
	for _, field := range allFields {
		value, err := c.Get(ctx, field)
		if err != nil {
			readErrs = append(readErrs, err.Error())
			continue
		}
		info.setField(field, value)
		if value != "" {
			info.Available = true
		}
	}

	if !info.Available && len(readErrs) > 0 {
		info.Error = readErrs[0]
		return info, errors.New(info.Error)
	}
	return info, nil
}

func (c Client) Get(ctx context.Context, field Field) (string, error) {
	normalizedField, err := NormalizeField(string(field))
	if err != nil {
		return "", err
	}

	out, err := c.run(ctx, "get", string(normalizedField))
	value := strings.TrimSpace(string(out))
	if err != nil {
		return "", fmt.Errorf("factory EEPROM get %s failed: %s: %w", normalizedField, value, err)
	}
	return value, nil
}

func (c Client) SetAndVerify(ctx context.Context, fieldName, value string) (Info, error) {
	field, err := NormalizeField(fieldName)
	if err != nil {
		return Info{Source: c.toolPath()}, err
	}
	normalizedValue, err := ValidateValue(field, value)
	if err != nil {
		return Info{Source: c.toolPath()}, err
	}

	out, err := c.run(ctx, "set", string(field), normalizedValue)
	if err != nil {
		return Info{Source: c.toolPath()}, fmt.Errorf("factory EEPROM set %s failed: %s: %w", field, strings.TrimSpace(string(out)), err)
	}

	readBack, err := c.Get(ctx, field)
	if err != nil {
		return Info{Source: c.toolPath()}, fmt.Errorf("factory EEPROM set %s cannot verify read-back: %w", field, err)
	}
	if !ValuesEqual(field, normalizedValue, readBack) {
		return Info{Source: c.toolPath()}, fmt.Errorf("factory EEPROM set %s verification mismatch: wrote %q read %q", field, normalizedValue, readBack)
	}

	info, readErr := c.Read(ctx)
	if readErr != nil {
		return info, fmt.Errorf("factory EEPROM set %s verified, but full read failed: %w", field, readErr)
	}
	return info, nil
}

func NormalizeField(raw string) (Field, error) {
	value := strings.ToUpper(strings.TrimSpace(raw))
	value = strings.ReplaceAll(value, "-", "_")
	switch value {
	case "SN", "SERIAL", "SERIAL_NUMBER":
		return FieldSN, nil
	case "MAC", "MAC_ADDRESS", "FACTORY_MAC", "FACTORY_MAC_ADDRESS":
		return FieldMAC, nil
	case "PN", "PRODUCT_NUMBER", "PART_NUMBER":
		return FieldPN, nil
	case "BATCH", "BATCH_NUMBER":
		return FieldBatch, nil
	case "HWREV", "HW_REV", "HARDWARE_REVISION", "HARDWARE_VERSION":
		return FieldHWRev, nil
	default:
		return "", fmt.Errorf("unsupported factory EEPROM field %q", raw)
	}
}

func ValidateValue(field Field, raw string) (string, error) {
	value := strings.TrimSpace(raw)
	if value == "" {
		return "", fmt.Errorf("%s cannot be empty", field)
	}

	if field == FieldMAC {
		mac, err := net.ParseMAC(value)
		if err != nil {
			return "", fmt.Errorf("invalid MAC address %q", raw)
		}
		return strings.ToLower(mac.String()), nil
	}

	maxLen := 0
	switch field {
	case FieldSN:
		maxLen = 32
	case FieldPN:
		maxLen = 16
	case FieldBatch:
		maxLen = 8
	case FieldHWRev:
		maxLen = 8
	default:
		return "", fmt.Errorf("unsupported factory EEPROM field %q", field)
	}
	if len(value) > maxLen {
		return "", fmt.Errorf("%s is too long: max %d bytes", field, maxLen)
	}
	if !asciiFactoryValueRE.MatchString(value) {
		return "", fmt.Errorf("%s may only contain letters, numbers, dot, underscore, colon, and hyphen", field)
	}
	return value, nil
}

func ValuesEqual(field Field, expected, actual string) bool {
	if field == FieldMAC {
		expectedMAC, expectedErr := net.ParseMAC(expected)
		actualMAC, actualErr := net.ParseMAC(actual)
		return expectedErr == nil && actualErr == nil && strings.EqualFold(expectedMAC.String(), actualMAC.String())
	}
	return strings.TrimSpace(expected) == strings.TrimSpace(actual)
}

func (c Client) toolPath() string {
	toolPath := strings.TrimSpace(c.ToolPath)
	if toolPath == "" {
		toolPath = DefaultToolPath
	}
	return toolPath
}

func (c Client) timeout() time.Duration {
	if c.Timeout > 0 {
		return c.Timeout
	}
	return 2 * time.Second
}

func (c Client) run(ctx context.Context, command string, args ...string) ([]byte, error) {
	runCtx, cancel := context.WithTimeout(ctx, c.timeout())
	defer cancel()

	allArgs := make([]string, 0, len(args)+3)
	if devicePath := strings.TrimSpace(c.DevicePath); devicePath != "" {
		allArgs = append(allArgs, "-d", devicePath)
	}
	allArgs = append(allArgs, command)
	allArgs = append(allArgs, args...)

	cmd := exec.CommandContext(runCtx, c.toolPath(), allArgs...)
	out, err := cmd.CombinedOutput()
	if runCtx.Err() != nil {
		return out, runCtx.Err()
	}
	return out, err
}

func (i *Info) setField(field Field, value string) {
	switch field {
	case FieldSN:
		i.SerialNumber = value
	case FieldMAC:
		i.MACAddress = value
	case FieldPN:
		i.ProductNumber = value
	case FieldBatch:
		i.Batch = value
	case FieldHWRev:
		i.HardwareRevision = value
	}
}
