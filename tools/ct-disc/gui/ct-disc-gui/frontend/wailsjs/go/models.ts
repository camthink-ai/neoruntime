export namespace main {
	
	export class DeviceItem {
	    sn: string;
	    product: string;
	    ip: string;
	    port: number;
	    fw: string;
	    caps: string[];
	    hw: string;
	    mac: string;
	    online: boolean;
	    lastSeen: string;
	    firstSeen: string;
	    manual?: boolean;
	
	    static createFrom(source: any = {}) {
	        return new DeviceItem(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.sn = source["sn"];
	        this.product = source["product"];
	        this.ip = source["ip"];
	        this.port = source["port"];
	        this.fw = source["fw"];
	        this.caps = source["caps"];
	        this.hw = source["hw"];
	        this.mac = source["mac"];
	        this.online = source["online"];
	        this.lastSeen = source["lastSeen"];
	        this.firstSeen = source["firstSeen"];
	        this.manual = source["manual"];
	    }
	}
	export class ListenerStats {
	    recvCount: number;
	    decodeErrs: number;
	    eventCount: number;
	    running: boolean;
	    ifaces: string[];
	
	    static createFrom(source: any = {}) {
	        return new ListenerStats(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.recvCount = source["recvCount"];
	        this.decodeErrs = source["decodeErrs"];
	        this.eventCount = source["eventCount"];
	        this.running = source["running"];
	        this.ifaces = source["ifaces"];
	    }
	}
	export class MetricRecord {
	    timestamp: string;
	    unix_ms: number;
	    sn?: string;
	    mac?: string;
	    product?: string;
	    ip?: string;
	    api_url: string;
	    online: boolean;
	    metrics_ok: boolean;
	    error?: string;
	    hostname?: string;
	    platform?: string;
	    uptime_seconds?: number;
	    cpu_percent: number;
	    memory_percent: number;
	    memory_used_bytes: number;
	    memory_total_bytes: number;
	    disk_percent: number;
	    disk_used_bytes: number;
	    disk_total_bytes: number;
	    disk_mountpoint?: string;
	    npu_percent: number;
	    temp_cpu: number;
	    temp_npu: number;
	    temp_board: number;
	    latency_ms: number;
	
	    static createFrom(source: any = {}) {
	        return new MetricRecord(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.timestamp = source["timestamp"];
	        this.unix_ms = source["unix_ms"];
	        this.sn = source["sn"];
	        this.mac = source["mac"];
	        this.product = source["product"];
	        this.ip = source["ip"];
	        this.api_url = source["api_url"];
	        this.online = source["online"];
	        this.metrics_ok = source["metrics_ok"];
	        this.error = source["error"];
	        this.hostname = source["hostname"];
	        this.platform = source["platform"];
	        this.uptime_seconds = source["uptime_seconds"];
	        this.cpu_percent = source["cpu_percent"];
	        this.memory_percent = source["memory_percent"];
	        this.memory_used_bytes = source["memory_used_bytes"];
	        this.memory_total_bytes = source["memory_total_bytes"];
	        this.disk_percent = source["disk_percent"];
	        this.disk_used_bytes = source["disk_used_bytes"];
	        this.disk_total_bytes = source["disk_total_bytes"];
	        this.disk_mountpoint = source["disk_mountpoint"];
	        this.npu_percent = source["npu_percent"];
	        this.temp_cpu = source["temp_cpu"];
	        this.temp_npu = source["temp_npu"];
	        this.temp_board = source["temp_board"];
	        this.latency_ms = source["latency_ms"];
	    }
	}
	export class NetworkConfig {
	    interface: string;
	    mode: string;
	    ip_address: string;
	    subnet_mask: string;
	    gateway: string;
	    dns1: string;
	    dns2: string;
	    mac: string;
	    sn: string;
	
	    static createFrom(source: any = {}) {
	        return new NetworkConfig(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.interface = source["interface"];
	        this.mode = source["mode"];
	        this.ip_address = source["ip_address"];
	        this.subnet_mask = source["subnet_mask"];
	        this.gateway = source["gateway"];
	        this.dns1 = source["dns1"];
	        this.dns2 = source["dns2"];
	        this.mac = source["mac"];
	        this.sn = source["sn"];
	    }
	}
	export class RecordStatus {
	    running: boolean;
	    outputPath: string;
	    format: string;
	    startedAt: string;
	    stoppedAt: string;
	    lastSampleAt: string;
	    lastError: string;
	    samplesWritten: number;
	    recordsWritten: number;
	    targetCount: number;
	
	    static createFrom(source: any = {}) {
	        return new RecordStatus(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.running = source["running"];
	        this.outputPath = source["outputPath"];
	        this.format = source["format"];
	        this.startedAt = source["startedAt"];
	        this.stoppedAt = source["stoppedAt"];
	        this.lastSampleAt = source["lastSampleAt"];
	        this.lastError = source["lastError"];
	        this.samplesWritten = source["samplesWritten"];
	        this.recordsWritten = source["recordsWritten"];
	        this.targetCount = source["targetCount"];
	    }
	}
	export class Settings {
	    mqttBroker: string;
	    mqttUser: string;
	    mqttPass: string;
	    interface: string;
	
	    static createFrom(source: any = {}) {
	        return new Settings(source);
	    }
	
	    constructor(source: any = {}) {
	        if ('string' === typeof source) source = JSON.parse(source);
	        this.mqttBroker = source["mqttBroker"];
	        this.mqttUser = source["mqttUser"];
	        this.mqttPass = source["mqttPass"];
	        this.interface = source["interface"];
	    }
	}

}

