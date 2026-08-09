#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Helper script to install emdoor-wmi using DKMS
#
# Usage: sudo ./dkms-install.sh
#        sudo ./dkms-install.sh remove
#        sudo ./dkms-install.sh status

set -euo pipefail

PACKAGE_NAME="emdoor-wmi"
VERSION=$(cat VERSION 2>/dev/null || echo "1.0.0")
DKMS_DIR="/usr/src/${PACKAGE_NAME}-${VERSION}"

usage() {
    cat <<EOF
Usage: sudo $0 [command]

Commands:
    install    Install the module via DKMS (default)
    remove     Remove the module from DKMS
    status     Show DKMS status for this module
    build      Build the module for current kernel only

Examples:
    sudo $0              # Install via DKMS
    sudo $0 remove       # Remove from DKMS
    sudo $0 status       # Check status
EOF
}

check_root() {
    if [[ $EUID -ne 0 ]]; then
        echo "This script must be run as root (use sudo)"
        exit 1
    fi
}

check_dkms() {
    if ! command -v dkms &> /dev/null; then
        echo "DKMS is not installed. Please install it first:"
        echo "  Debian/Ubuntu: apt install dkms"
        echo "  Fedora:        dnf install dkms"
        echo "  Arch:          pacman -S dkms"
        exit 1
    fi
}

cmd_install() {
    check_root
    check_dkms

    echo "Installing ${PACKAGE_NAME}-${VERSION} via DKMS..."

    # Copy source to DKMS tree
    if [[ -d "${DKMS_DIR}" ]]; then
        echo "Removing existing DKMS source at ${DKMS_DIR}"
        dkms remove -m "${PACKAGE_NAME}" -v "${VERSION}" --all 2>/dev/null || true
        rm -rf "${DKMS_DIR}"
    fi

    echo "Copying source to ${DKMS_DIR}..."
    mkdir -p "${DKMS_DIR}"
    cp -r . "${DKMS_DIR}/"
    # Remove build artifacts from copied source
    find "${DKMS_DIR}" -name '*.o' -o -name '*.ko' -o -name '*.mod*' -o -name 'Module.symvers' -o -name 'modules.order' | xargs rm -f 2>/dev/null || true
    rm -rf "${DKMS_DIR}/.git" "${DKMS_DIR}/.clio" 2>/dev/null || true

    echo "Adding to DKMS..."
    dkms add -m "${PACKAGE_NAME}" -v "${VERSION}"

    echo "Building for current kernel..."
    dkms build -m "${PACKAGE_NAME}" -v "${VERSION}"

    echo "Installing..."
    dkms install -m "${PACKAGE_NAME}" -v "${VERSION}"

    echo "Done! Module installed via DKMS."
    echo "It will be automatically rebuilt on kernel updates."
}

cmd_remove() {
    check_root
    check_dkms

    echo "Removing ${PACKAGE_NAME}-${VERSION} from DKMS..."
    dkms remove -m "${PACKAGE_NAME}" -v "${VERSION}" --all
    rm -rf "${DKMS_DIR}"
    echo "Done."
}

cmd_status() {
    check_dkms
    dkms status -m "${PACKAGE_NAME}" -v "${VERSION}"
}

cmd_build() {
    check_root
    check_dkms

    echo "Building ${PACKAGE_NAME}-${VERSION} for current kernel..."
    dkms build -m "${PACKAGE_NAME}" -v "${VERSION}"
}

# Main
COMMAND="${1:-install}"

case "${COMMAND}" in
    install) cmd_install ;;
    remove)  cmd_remove ;;
    status)  cmd_status ;;
    build)   cmd_build ;;
    -h|--help|help) usage ;;
    *) echo "Unknown command: ${COMMAND}"; usage; exit 1 ;;
esac