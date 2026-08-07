//go:build !windows

package discover

import (
	"fmt"
	"net"
	"os/exec"
)

func addHostRoute(targetIP net.IP, srcIP net.IP) error {
	iface, err := findInterfaceByIP(srcIP)
	if err != nil {
		return err
	}
	cmd := exec.Command("ip", "route", "add",
		fmt.Sprintf("%s/32", targetIP), "dev", iface.Name,
	)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("ip route add failed: %s: %w", string(out), err)
	}
	return nil
}

func removeHostRoute(targetIP net.IP) error {
	cmd := exec.Command("ip", "route", "del", fmt.Sprintf("%s/32", targetIP))
	cmd.Run()
	return nil
}

func findInterfaceByIP(target net.IP) (*net.Interface, error) {
	ifaces, err := net.Interfaces()
	if err != nil {
		return nil, err
	}
	for _, iface := range ifaces {
		addrs, _ := iface.Addrs()
		for _, addr := range addrs {
			if ipnet, ok := addr.(*net.IPNet); ok && ipnet.IP.Equal(target) {
				return &iface, nil
			}
		}
	}
	return nil, fmt.Errorf("no interface with IP %s", target)
}
