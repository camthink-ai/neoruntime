package events

import (
	"bytes"
	"context"
	"fmt"
	"strings"
	"sync"
	"text/template"
)

// Localizer handles message localization
type Localizer struct {
	defaultLocale string
}

var (
	globalLocalizer     *Localizer
	globalLocalizerOnce sync.Once
)

// GetLocalizer returns the global localizer instance
func GetLocalizer() *Localizer {
	globalLocalizerOnce.Do(func() {
		globalLocalizer = &Localizer{
			defaultLocale: "zh-CN",
		}
	})
	return globalLocalizer
}

// SetDefaultLocale sets the default locale for the localizer
func (l *Localizer) SetDefaultLocale(locale string) {
	l.defaultLocale = locale
}

// Localize returns a localized message for the given code and parameters
func (l *Localizer) Localize(code string, params MessageParams, locale string) string {
	// Use provided locale or fall back to default
	if locale == "" {
		locale = l.defaultLocale
	}

	// Normalize locale (e.g., "zh-CN" -> "zh_CN" for internal map)
	localeKey := strings.ReplaceAll(locale, "-", "_")

	// Get the template
	tmpl := GetTemplate(code)
	if tmpl == nil {
		return code // Fallback to code if template not found
	}

	// Get translated message format
	var messageFormat string
	if localeKey == "zh_CN" {
		if msg, ok := zhCNMessages[code]; ok {
			messageFormat = msg
		} else {
			messageFormat = tmpl.Default
		}
	} else {
		// Default to English for all other locales
		messageFormat = tmpl.Default
	}

	// If no parameters, return the format as-is
	if len(params) == 0 {
		return messageFormat
	}

	// Apply special value mappings for Chinese
	if localeKey == "zh_CN" {
		if valueMaps, ok := zhCNSpecialValues[code]; ok {
			params = l.applyValueMappings(params, valueMaps)
		}
	}

	// Format the message with parameters
	return l.formatMessage(messageFormat, params)
}

// formatMessage formats a message template with parameters using Go templates.
// Missing parameters are replaced with empty strings instead of "<no value>".
func (l *Localizer) formatMessage(format string, params MessageParams) string {
	tmpl, err := template.New("msg").Option("missingkey=zero").Parse(format)
	if err != nil {
		// Fallback to simple string replacement
		result := format
		for key, value := range params {
			placeholder := fmt.Sprintf("{{.%s}}", key)
			result = strings.ReplaceAll(result, placeholder, fmt.Sprintf("%v", value))
		}
		return result
	}

	var buf bytes.Buffer
	err = tmpl.Execute(&buf, params)
	if err != nil {
		return format
	}

	return buf.String()
}

// applyValueMappings applies special value mappings for certain parameters
func (l *Localizer) applyValueMappings(params MessageParams, valueMaps map[string]map[string]string) MessageParams {
	result := make(MessageParams)
	for k, v := range params {
		result[k] = v
	}

	// Apply mappings
	for paramKey, mappings := range valueMaps {
		if val, ok := params[paramKey]; ok {
			valStr := fmt.Sprintf("%v", val)
			if mapped, ok := mappings[valStr]; ok {
				result[paramKey] = mapped
			}
		}
	}

	return result
}

// LocalizeFromContext extracts locale from context and localizes the message
func (l *Localizer) LocalizeFromContext(ctx context.Context, code string, params MessageParams) string {
	locale := l.localeFromContext(ctx)
	return l.Localize(code, params, locale)
}

// localeFromContext extracts locale from request context
func (l *Localizer) localeFromContext(ctx context.Context) string {
	if ctx == nil {
		return l.defaultLocale
	}

	// Try to get locale from context value (set by middleware)
	if locale, ok := ctx.Value("locale").(string); ok {
		return locale
	}

	// Try Accept-Language header
	if header, ok := ctx.Value("accept-language").(string); ok {
		return l.parseAcceptLanguage(header)
	}

	return l.defaultLocale
}

// parseAcceptLanguage parses Accept-Language header and returns the best locale
func (l *Localizer) parseAcceptLanguage(header string) string {
	// Simple parser - takes the first language
	// e.g., "zh-CN,zh;q=0.9,en;q=0.8" -> "zh-CN"
	parts := strings.Split(header, ",")
	if len(parts) > 0 {
		locale := strings.TrimSpace(parts[0])
		// Remove q-value if present
		if idx := strings.Index(locale, ";"); idx > 0 {
			locale = locale[:idx]
		}
		return locale
	}
	return l.defaultLocale
}

// LocalizeEvent returns a localized message for an Event
func LocalizeEvent(event *Event, locale string) string {
	return GetLocalizer().Localize(string(event.Type), event.Data, locale)
}

// Helper functions for common log codes

// LogWithCode is a convenience method that uses event code instead of message string
// The message is stored as the event code for frontend-side i18n translation.
func (l *Logger) LogWithCode(
	code string,
	params MessageParams,
	source, user string,
) {
	tmpl := GetTemplate(code)
	if tmpl == nil {
		return
	}

	l.LogEvent(
		EventType(code),
		tmpl.Level,
		tmpl.Category,
		source, user,
		code,
		params,
	)
}

// LogWithCodeAsync logs an event using code-based message template
// Module name and severity are automatically read from the template.
// The message is stored as the event code for frontend-side i18n translation.
func (l *Logger) LogWithCodeAsync(
	code string,
	params MessageParams,
	user string,
) {
	tmpl := GetTemplate(code)
	if tmpl == nil {
		return
	}

	l.LogEvent(
		EventType(code),
		tmpl.Level,
		tmpl.Category,
		tmpl.Module, user,
		code,
		params,
	)
}
