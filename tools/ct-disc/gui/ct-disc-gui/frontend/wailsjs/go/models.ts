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

