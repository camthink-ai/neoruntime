package events

// zhCNMessages contains Chinese (Simplified) translations
var zhCNMessages = map[string]string{
	// User & Auth
	"user.login.success": "用户 {{.username}} 从 {{.ip}} 登录",
	"user.login.failed":  "用户 {{.username}} 登录失败: {{.reason}}，来源 IP: {{.ip}}",
	"user.logout":        "用户 {{.username}} 登出",

	// Config changes
	"config.changed":   "配置已更改: {{.section}} - {{.changes}}",
	"password.changed": "用户 {{.username}} 密码已更改",

	// Network
	"network.config.changed": "网络配置已更新: {{.interface}} ({{.mode}}, {{.ip}})",
	"network.connected":      "网络已连接: {{.interface}} @ {{.ip}}",
	"network.disconnected":   "网络已断开: {{.interface}}",

	// Storage
	"storage.disk.mounted":   "磁盘已挂载: {{.device}} 挂载至 {{.target}}",
	"storage.disk.unmounted": "磁盘已卸载: {{.target}}",
	"storage.disk.formatted": "磁盘已格式化: {{.device}} 格式化为 {{.fstype}}",
	"storage.low":            "存储空间不足: {{.path}} 使用率 {{.usage_percent}}%",
	"storage.full":           "存储空间已满: {{.path}}",

	// Time
	"time.manual_set":         "系统时间已手动设置为 {{.datetime}}",
	"time.timezone.changed":   "时区已更改为 {{.timezone}}",
	"time.ntp.config_changed": "NTP 配置已更新: 启用={{.enabled}}, 服务器={{.server}}",
	"time.ntp.sync_triggered": "NTP 同步已触发",
	"time.config.saved":       "时间配置已保存: 时区={{.timezone}}, 同步模式={{.sync_mode}}",
	"time.client_sync":        "客户端时间已同步: 偏差={{.diff_seconds}}秒",
	"ntp.sync.failed":         "NTP 同步失败: {{.server}} - {{.error}}",

	// SSH
	"ssh.config.changed": "SSH 配置已更改: {{.changes}}",
	"ssh.login.success":  "SSH 登录: {{.username}} 从 {{.ip}}",
	"ssh.login.failed":   "SSH 登录失败: {{.username}} 从 {{.ip}}",

	// AI / Models
	"ai.model.uploaded":   "AI 模型已上传: {{.model_id}} ({{.filename}}, {{.size}} 字节)",
	"ai.model.loaded":     "AI 模型已加载: {{.model_id}} 来自 {{.model_path}}",
	"ai.model.unloaded":   "AI 模型已卸载: {{.model_id}}",
	"ai.model.deleted":    "AI 模型已删除: {{.model_id}}",
	"ai.inference.failed": "AI 推理失败: {{.model_id}} - {{.error}}",

	// Apps / Containers
	"app.installed":     "应用已安装: {{.app_id}} 版本 {{.version}}",
	"app.started":       "应用已启动: {{.app_id}}",
	"app.stopped":       "应用已停止: {{.app_id}} - {{.reason}}",
	"app.uninstalled":   "应用已卸载: {{.app_id}}",
	"app.crashed":       "应用崩溃: {{.app_id}} - {{.error}}",
	"container.started": "容器已启动: {{.container_id}} 对应应用 {{.app_id}}",
	"container.stopped": "容器已停止: {{.container_id}} (退出码: {{.exit_code}})",

	// Device control
	"device.control":           "设备控制: {{.device}} - {{.action}}",
	"device.ptz.control":       "云台控制: {{.action}} 方向={{.direction}} 速度={{.speed}}",
	"device.ptz.moved":         "云台已移动: 水平={{.pan}}, 垂直={{.tilt}}, 缩放={{.zoom}}",
	"device.zoom.changed":      "变焦已更改: 速度={{.speed}}",
	"device.focus.changed":     "对焦已更改: 速度={{.speed}}",
	"device.autofocus.changed": "自动对焦 {{.enable}}",
	"device.lens.oneshot_af":   "单次自动对焦已触发",
	"device.gpio.write":        "GPIO 写入: 引脚 {{.pin}} = {{.value}}",
	"device.lens.init":         "镜头 {{.action}}",
	"device.lens.goto":         "镜头定位: 缩放={{.zoom_ratio}}x 对焦={{.focus_distance_m}}m",
	"device.io.triggered":      "IO 触发: 端口 {{.port}} = {{.state}}",

	// Media / Camera
	"media.config.changed":        "媒体配置已更改: {{.stream}} - {{.changes}}",
	"media.osd.changed":           "OSD 配置已更新，影响 {{.streams}} 个流",
	"media.ai_overlay.changed":    "AI 叠加层已{{.enabled}}",
	"camera.stream.started":       "摄像头流已启动: {{.stream_name}} ({{.resolution}})",
	"camera.stream.stopped":       "摄像头流已停止: {{.stream_name}} - {{.reason}}",
	"media.pipeline.reconfigured": "媒体流水线已重配置: {{.stream_count}} 个流",
	"media.stream.added":          "媒体流已添加: {{.stream}}",
	"media.stream.removed":        "媒体流已移除: {{.stream}}",
	"media.stream.enabled":        "媒体流已启用: {{.stream}}",
	"media.stream.disabled":       "媒体流已禁用: {{.stream}}",

	// System
	"system.reboot":     "系统重启由 {{.triggered_by}} 发起: {{.reason}}",
	"system.shutdown":   "系统关机由 {{.triggered_by}} 发起",
	"service.started":   "服务已启动: {{.service_name}}",
	"service.stopped":   "服务已停止: {{.service_name}}",
	"service.failed":    "服务失败: {{.service_name}} - {{.error}}",
	"service.restarted": "服务已重启: {{.service_name}}",

	// Firmware
	"firmware.updated":        "固件已更新至 {{.version}} (原版本 {{.previous_version}})",
	"firmware.update.failed":  "固件更新失败: {{.version}} - {{.error}}",
	"firmware.update.started": "固件更新开始: {{.version}}",

	// Security
	"security.auth.failed":       "认证失败: {{.username}} 来自 {{.ip}} - {{.reason}}",
	"security.permission.denied": "权限拒绝: {{.user}} 尝试 {{.action}} 于 {{.resource}}",
	"security.ssl.cert.expiring": "SSL 证书即将过期: {{.certificate}} (剩余 {{.days_remaining}} 天)",

	// Hardware
	"hardware.temperature.high":     "温度过高: {{.device}} 温度 {{.temperature}}°C (阈值: {{.threshold}}°C)",
	"hardware.temperature.critical": "温度严重: {{.device}} 温度 {{.temperature}}°C",
	"hardware.fan.failed":           "风扇故障: {{.fan_id}}",
}

// GetZhMessage returns the Chinese message template for a given code.
// Returns empty string if no translation exists.
func GetZhMessage(code string) string {
	if msg, ok := zhCNMessages[code]; ok {
		return msg
	}
	return ""
}

// GetZhSpecialValues returns special value mappings for Chinese locale.
func GetZhSpecialValues(code string) map[string]map[string]string {
	if valueMaps, ok := zhCNSpecialValues[code]; ok {
		return valueMaps
	}
	return nil
}

// zhCNSpecialValues maps parameter values to Chinese translations
// Format: code -> paramKey -> paramValue -> translatedValue
var zhCNSpecialValues = map[string]map[string]map[string]string{
	"media.ai_overlay.changed": {
		"enabled": {
			"true":  "启用",
			"false": "禁用",
		},
	},
	"device.autofocus.changed": {
		"enable": {
			"true":  "已启用",
			"false": "已禁用",
		},
	},
}
