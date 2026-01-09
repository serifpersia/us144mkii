#!/bin/bash

LOG_FILE=$(mktemp)
MODULE_NAME="snd_usb_us122mkii"
KO_FILE="snd-usb-us122mkii.ko"
TARGET_DIR="/lib/modules/$(uname -r)/extra/us122mkii"

GREEN='\033[0;32m'
RED='\033[0;31m'
GRAY='\033[0;90m'
NC='\033[0m'
BOLD='\033[1m'

cleanup() {
    tput cnorm
    rm -f "$LOG_FILE"
}
trap cleanup EXIT

header() {
    clear
    echo -e "${GREEN}${BOLD}"
    echo "  TASCAM US-122MKII DRIVER INSTALLER"
    echo "  =================================="
    echo -e "${NC}"
}

init_sudo() {
    if [ "$EUID" -ne 0 ]; then
        echo -e "${GRAY}  Creating sudo session...${NC}"
        if ! sudo -v; then
            echo -e "\n  ${RED}✖ Authentication failed.${NC}"
            exit 1
        fi
    fi
}

run() {
    local cmd="$1"
    local msg="$2"
    local allow_fail="${3:-false}"

    tput civis
    eval "$cmd" > "$LOG_FILE" 2>&1 &
    local pid=$!

    local spin='⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏'
    local i=0

    while kill -0 "$pid" 2>/dev/null; do
        i=$(((i + 1) % ${#spin}))
        printf "\r  ${GREEN}${spin:$i:1}${NC} ${GRAY}%s...${NC}" "$msg"
        sleep 0.08
    done

    wait $pid
    local exit_code=$?

    if [ $exit_code -eq 0 ]; then
        printf "\r\033[K  ${GREEN}✔${NC} ${BOLD}%s${NC}\n" "$msg"
    else
        if [ "$allow_fail" = "true" ]; then
            printf "\r\033[K  ${GREEN}✔${NC} ${BOLD}%s${NC} ${GRAY}(Skipped)${NC}\n" "$msg"
        else
            printf "\r\033[K  ${RED}✖${NC} ${BOLD}%s${NC}\n" "$msg"
            echo -e "\n${RED}  Build Log:${NC}"
            echo "  ----------------------------------------"
            sed 's/^/  /' "$LOG_FILE"
            echo "  ----------------------------------------"
            exit 1
        fi
    fi
}

header
init_sudo

run "make clean" "Cleaning build directory"
run "make" "Compiling driver source"

run "sudo mkdir -p \"$TARGET_DIR\"" "Verifying module directory"
run "sudo cp \"$KO_FILE\" \"$TARGET_DIR\"" "Installing kernel module"
run "sudo depmod -a" "Updating dependency map"

run "sudo rmmod $MODULE_NAME" "Unloading previous driver" "true"
run "sudo modprobe $(echo $MODULE_NAME | tr '_' '-')" "Loading new driver"

echo -e "\n  ${GREEN}${BOLD}Done.${NC}\n"
