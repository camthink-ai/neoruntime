#!/bin/bash
# AIPC Platform - Installation Script

set -e

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

# Configuration
INSTALL_PREFIX="${INSTALL_PREFIX:-/data/aipc}"
SYSTEMD_DIR="/etc/systemd/system"
AIPC_NGINX_ROOT="${AIPC_NGINX_ROOT:-/data/nginx}"

os_manages_aipc_boot() {
    grep -qs '^os$' /etc/aipc-bootstrap-owner 2>/dev/null || \
        grep -qs '^AIPC_BOOTSTRAP_OWNER=os$' /etc/aipc-os-release
}

configure_platform_api_gateway_mode() {
    local mode="$1" helper="${INSTALL_PREFIX}/scripts/aipc-configure-platform-api-gateway.py"
    [ -x "$helper" ] && [ -f "${INSTALL_PREFIX}/etc/platform-api.yaml" ] || return 0
    /usr/bin/python3 "$helper" --config "${INSTALL_PREFIX}/etc/platform-api.yaml" --mode "$mode" || \
        echo -e "${YELLOW}⚠ Failed to configure platform-api gateway mode: ${mode}${NC}"
}

install_nginx_gateway_from_source() {
    local source="deploy/nginx" file name target
    [ -d "$source" ] || return 0

    mkdir -p \
        "${AIPC_NGINX_ROOT}/bin" "${AIPC_NGINX_ROOT}/conf" "${AIPC_NGINX_ROOT}/sbin" \
        "${AIPC_NGINX_ROOT}/run" "${AIPC_NGINX_ROOT}/logs" \
        "${AIPC_NGINX_ROOT}/tmp/client_body" "${AIPC_NGINX_ROOT}/tmp/proxy" \
        "${AIPC_NGINX_ROOT}/tmp/fastcgi" "${AIPC_NGINX_ROOT}/tmp/uwsgi" \
        "${AIPC_NGINX_ROOT}/tmp/scgi"

    if [ -d "$source/runtime" ]; then
        if [ -d "$source/runtime/bin" ]; then
            cp -aP "$source/runtime/bin"/. "${AIPC_NGINX_ROOT}/bin"/
            chmod 0755 "${AIPC_NGINX_ROOT}/bin/nginx" 2>/dev/null || true
        fi
        if [ -d "$source/runtime/rootfs" ]; then
            rm -rf "${AIPC_NGINX_ROOT}/rootfs"
            mkdir -p "${AIPC_NGINX_ROOT}/rootfs"
            cp -aP "$source/runtime/rootfs"/. "${AIPC_NGINX_ROOT}/rootfs"/
            chmod 0755 "${AIPC_NGINX_ROOT}/rootfs/usr/sbin/nginx" 2>/dev/null || true
        fi
    fi

    if [ -d "$source/conf" ]; then
        for file in "$source"/conf/*; do
            [ -f "$file" ] || continue
            name=$(basename "$file")
            target="${AIPC_NGINX_ROOT}/conf/${name}"
            if [[ "$name" == *.seed ]]; then
                target="${AIPC_NGINX_ROOT}/conf/${name%.seed}"
                [ -s "$target" ] && continue
            fi
            install -m 0644 "$file" "$target"
        done
    fi

    if [ -d "$source/sbin" ]; then
        for file in "$source"/sbin/*; do
            [ -f "$file" ] || continue
            install -m 0755 "$file" "${AIPC_NGINX_ROOT}/sbin/$(basename "$file")"
        done
    fi

    if [ -x "${AIPC_NGINX_ROOT}/bin/nginx" ]; then
        configure_platform_api_gateway_mode nginx
        if [ -x "${AIPC_NGINX_ROOT}/sbin/aipc-nginx-app-route-sync.py" ]; then
            /usr/bin/python3 "${AIPC_NGINX_ROOT}/sbin/aipc-nginx-app-route-sync.py" --ensure-cert >/dev/null 2>&1 || \
                echo -e "${YELLOW}⚠ Initial nginx route generation failed; the gateway service will retry${NC}"
        fi
    fi

    for legacy in nginx-data.service aipc-nginx-app-routes.service; do
        systemctl disable --now "$legacy" 2>/dev/null || true
        rm -f "${SYSTEMD_DIR}/${legacy}"
    done
}

echo -e "${GREEN}===================================${NC}"
echo -e "${GREEN}AIPC Platform Installation${NC}"
echo -e "${GREEN}===================================${NC}"
echo -e "\nInstall prefix: ${INSTALL_PREFIX}\n"

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo -e "${RED}Please run as root${NC}"
    exit 1
fi

# Create directories
echo -e "${YELLOW}[1/6] Creating directories...${NC}"
mkdir -p ${INSTALL_PREFIX}/{bin,lib/hal,etc,logs,data,models}
mkdir -p ${INSTALL_PREFIX}/scripts
mkdir -p ${INSTALL_PREFIX}/libexec
mkdir -p ${INSTALL_PREFIX}/recovery
install -m 0755 scripts/aipc-os-layout-check.sh ${INSTALL_PREFIX}/scripts/aipc-os-layout-check.sh
install -m 0755 scripts/aipc-install-current-root.sh ${INSTALL_PREFIX}/scripts/aipc-install-current-root.sh
install -m 0755 scripts/aipc-configure-platform-api-gateway.py ${INSTALL_PREFIX}/scripts/aipc-configure-platform-api-gateway.py
if [ -d build/recovery/hailo15-ne503 ]; then
    install -m 0644 build/recovery/hailo15-ne503/* ${INSTALL_PREFIX}/recovery/
fi
mkdir -p /run/aipc/{shm,sockets}
mkdir -p /usr/libexec
install -m 0755 scripts/aipc-compat-check.sh ${INSTALL_PREFIX}/libexec/aipc-compat-check
ln -sfn ${INSTALL_PREFIX}/libexec/aipc-compat-check /usr/libexec/aipc-compat-check

if ! os_manages_aipc_boot; then
    install -m 0755 scripts/aipc-restore.sh /usr/libexec/aipc-restore
    install -m 0755 scripts/aipc-firstboot-os.sh /usr/libexec/aipc-firstboot
    install -m 0755 scripts/aipc-autostart.sh /usr/libexec/aipc-autostart
fi
echo "✓ Directories created"

# Install binaries
echo -e "\n${YELLOW}[2/6] Installing binaries...${NC}"
if [ -d "build/output" ]; then
    cp build/output/* ${INSTALL_PREFIX}/bin/ 2>/dev/null || true
    chmod +x ${INSTALL_PREFIX}/bin/*
    if [ -f "build/output/aipc-os-updater" ]; then
        install -m 0755 build/output/aipc-os-updater ${INSTALL_PREFIX}/libexec/aipc-os-updater
        ln -sfn ${INSTALL_PREFIX}/libexec/aipc-os-updater /usr/libexec/aipc-os-updater
    fi
    echo "✓ Binaries installed"
else
    echo "⚠ Build output not found, skipping"
fi

# Install HAL v2 libraries
echo -e "\n${YELLOW}[3/6] Installing HAL libraries...${NC}"
HAL_PLATFORM="${HAL_PLATFORM:-stub}"
HAL_LIB_SOURCE=""
for candidate in "build/output/hal/${HAL_PLATFORM}" "build/output/hal/stub" "build/output/hal/hailo15"; do
    if [ -d "$candidate" ] && compgen -G "$candidate/libaipc_hal*.so*" >/dev/null; then
        HAL_LIB_SOURCE="$candidate"
        break
    fi
done

if [ -n "$HAL_LIB_SOURCE" ]; then
    cp -aP "$HAL_LIB_SOURCE"/libaipc_hal*.so* "$HAL_LIB_SOURCE"/libhal-*.so* ${INSTALL_PREFIX}/lib/hal/ 2>/dev/null || true
    echo "✓ HAL v2 libraries installed from ${HAL_LIB_SOURCE}"
else
    echo "⚠ HAL v2 libraries not found, run: make hal-v2 HAL_PLATFORM=${HAL_PLATFORM}"
fi

# Configure ldconfig to find HAL libraries
mkdir -p /etc/ld.so.conf.d
echo "${INSTALL_PREFIX}/lib/hal" > /etc/ld.so.conf.d/aipc.conf
ldconfig 2>/dev/null || true
echo "✓ HAL library path configured in ldconfig"

# Install configurations
echo -e "\n${YELLOW}[4/6] Installing configurations...${NC}"
cp -r configs/* ${INSTALL_PREFIX}/etc/
mkdir -p ${INSTALL_PREFIX}/etc/security
cp configs/security/* ${INSTALL_PREFIX}/etc/security/ 2>/dev/null || true
mkdir -p \
    ${INSTALL_PREFIX}/etc/systemd/system.conf.d \
    ${INSTALL_PREFIX}/etc/systemd/journald.conf.d \
    ${INSTALL_PREFIX}/etc/sysctl.d
cp configs/systemd/*.conf ${INSTALL_PREFIX}/etc/systemd/system.conf.d/ 2>/dev/null || true
cp configs/systemd/journald.conf.d/*.conf ${INSTALL_PREFIX}/etc/systemd/journald.conf.d/ 2>/dev/null || true
cp configs/system/sysctl.d/*.conf ${INSTALL_PREFIX}/etc/sysctl.d/ 2>/dev/null || true

# Install seccomp profile to system location (required by app-manager)
mkdir -p /etc/aipc
cp configs/security/seccomp-default.json /etc/aipc/seccomp-default.json
echo "✓ Configurations installed (seccomp -> /etc/aipc/)"

install_nginx_gateway_from_source
echo "✓ Nginx app gateway installed (${AIPC_NGINX_ROOT}/)"

# Install Swagger UI and API documentation
echo -e "\n${YELLOW}[5/6] Installing Swagger UI...${NC}"
mkdir -p ${INSTALL_PREFIX}/swagger-ui
cp platform/platform-api/swagger-ui/* ${INSTALL_PREFIX}/swagger-ui/ 2>/dev/null || true
cp docs/api/swagger.yaml ${INSTALL_PREFIX}/etc/swagger.yaml 2>/dev/null || true
echo "✓ Swagger UI installed (${INSTALL_PREFIX}/swagger-ui/)"

# Install systemd services
echo -e "\n${YELLOW}[6/6] Installing systemd services...${NC}"
for unit in systemd/*.service systemd/*.timer systemd/*.target; do
    [ -f "$unit" ] || continue
    name=$(basename "$unit")
    if os_manages_aipc_boot; then
        case "$name" in
            aipc-restore.service|aipc-firstboot.service|aipc-autostart.service|aipc-os-verify.service)
                rm -f "${SYSTEMD_DIR}/${name}"
                continue
                ;;
        esac
    fi
    cp "$unit" "${SYSTEMD_DIR}/"
done
systemctl daemon-reload
if os_manages_aipc_boot; then
    systemctl reenable aipc-restore.service aipc-firstboot.service aipc-autostart.service aipc-os-verify.service 2>/dev/null || true
else
    systemctl enable aipc-restore.service aipc-firstboot.service aipc-autostart.service aipc-os-verify.service 2>/dev/null || true
fi
echo "✓ Systemd services installed"

# Publish the canonical unit set as one directory rename. This mirrors the
# release layout used by deploy.sh and never exposes a partially refreshed set.
canonical_units_tmp="${INSTALL_PREFIX}/.systemd-stage-$$"
rm -rf "$canonical_units_tmp"
mkdir -p "$canonical_units_tmp"
for unit in systemd/*.service systemd/*.timer systemd/*.target; do
    [ -f "$unit" ] || continue
    install -m 0644 "$unit" "$canonical_units_tmp/"
done
[ -f "$canonical_units_tmp/aipc-platform.target" ] || {
    echo -e "${RED}Canonical aipc-platform.target is missing${NC}" >&2
    exit 1
}
if [ -d "${INSTALL_PREFIX}/systemd" ]; then
    if [ ! -x "${INSTALL_PREFIX}/libexec/aipc-os-updater" ]; then
        echo -e "${RED}Atomic swap helper is missing; refusing to replace canonical units${NC}" >&2
        exit 1
    fi
    "${INSTALL_PREFIX}/libexec/aipc-os-updater" exchange-dirs \
        "${INSTALL_PREFIX}/systemd" "$canonical_units_tmp"
    rm -rf "$canonical_units_tmp"
else
    mv "$canonical_units_tmp" "${INSTALL_PREFIX}/systemd"
fi

# Summary
echo -e "\n${GREEN}===================================${NC}"
echo -e "${GREEN}Installation Complete!${NC}"
echo -e "${GREEN}===================================${NC}"

echo -e "\nNext steps:"
echo -e "  1. Enable services:"
echo -e "     ${YELLOW}systemctl enable aipc-platform.target${NC}"
echo -e "  2. Start services:"
echo -e "     ${YELLOW}systemctl start aipc-platform.target${NC}"
echo -e "  3. Check status:"
echo -e "     ${YELLOW}systemctl status aipc-platform.target${NC}"
echo -e "  4. View logs:"
echo -e "     ${YELLOW}journalctl -u 'aipc-*' -f${NC}"

echo -e "\n${GREEN}Installation successful! 🚀${NC}\n"
