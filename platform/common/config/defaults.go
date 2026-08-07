package config

import (
	"fmt"
	"reflect"
	"time"
)

// SetDefaults sets default values for struct fields using struct tags
// Usage: SetDefaults(&cfg, "default")
func SetDefaults(cfg interface{}) {
	v := reflect.ValueOf(cfg)
	if v.Kind() != reflect.Ptr {
		return
	}

	v = v.Elem()
	if v.Kind() != reflect.Struct {
		return
	}

	setDefaultsRecursive(v)
}

func setDefaultsRecursive(v reflect.Value) {
	t := v.Type()

	for i := 0; i < v.NumField(); i++ {
		field := v.Field(i)
		fieldType := t.Field(i)

		// Skip unexported fields
		if !field.CanSet() {
			continue
		}

		// Handle nested structs
		if field.Kind() == reflect.Struct {
			setDefaultsRecursive(field)
			continue
		}

		// Handle pointers to structs
		if field.Kind() == reflect.Ptr && field.IsNil() {
			if fieldType.Type.Elem().Kind() == reflect.Struct {
				field.Set(reflect.New(fieldType.Type.Elem()))
				setDefaultsRecursive(field.Elem())
			}
			continue
		}

		// Get default value from tag
		defaultTag := fieldType.Tag.Get("default")
		if defaultTag == "" {
			continue
		}

		// Only set if field is zero value
		if !isZeroValue(field) {
			continue
		}

		setFieldValue(field, defaultTag)
	}
}

func isZeroValue(v reflect.Value) bool {
	switch v.Kind() {
	case reflect.String:
		return v.String() == ""
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		return v.Int() == 0
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64:
		return v.Uint() == 0
	case reflect.Float32, reflect.Float64:
		return v.Float() == 0
	case reflect.Bool:
		return !v.Bool()
	case reflect.Ptr, reflect.Interface, reflect.Slice, reflect.Map:
		return v.IsNil()
	default:
		return false
	}
}

func setFieldValue(field reflect.Value, value string) {
	switch field.Kind() {
	case reflect.String:
		field.SetString(value)
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		if intVal := parseInt(value); intVal != nil {
			field.SetInt(*intVal)
		}
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64:
		if uintVal := parseUint(value); uintVal != nil {
			field.SetUint(*uintVal)
		}
	case reflect.Bool:
		if boolVal := parseBool(value); boolVal != nil {
			field.SetBool(*boolVal)
		}
	default:
		// Check if field type is time.Duration
		if field.Type().String() == "time.Duration" {
			if dur, err := time.ParseDuration(value); err == nil {
				field.Set(reflect.ValueOf(dur))
			}
		}
	}
}

func parseInt(s string) *int64 {
	var val int64
	if _, err := fmt.Sscanf(s, "%d", &val); err == nil {
		return &val
	}
	return nil
}

func parseUint(s string) *uint64 {
	var val uint64
	if _, err := fmt.Sscanf(s, "%d", &val); err == nil {
		return &val
	}
	return nil
}

func parseBool(s string) *bool {
	switch s {
	case "true", "1", "yes", "on":
		val := true
		return &val
	case "false", "0", "no", "off":
		val := false
		return &val
	}
	return nil
}
