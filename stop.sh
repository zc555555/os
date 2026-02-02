#!/bin/bash

# Color definitions
BLUE='\033[0;94m'
GREEN='\033[0;92m'
RED='\033[0;91m'
YELLOW='\033[0;93m'
NC='\033[0m' # No Color

info() {
    echo -e "${BLUE}[*] $1${NC}"
}

success() {
    echo -e "${GREEN}[+] $1${NC}"
}

error() {
    echo -e "${RED}[-] $1${NC}"
}

warning() {
    echo -e "${YELLOW}[!] $1${NC}"
}

info "Searching for running QEMU instances..."

# Get QEMU processes
QEMU_PROCS=$(ps aux | grep -E 'qemu-system' | grep -v grep)

if [ -z "$QEMU_PROCS" ]; then
    warning "No running QEMU instances found."
    exit 0
fi

# Display running instances
echo ""
echo "Running QEMU instances:"
echo "------------------------"

# Create arrays to store PIDs and info
declare -a PIDS
declare -a INFOS
i=1

while IFS= read -r line; do
    PID=$(echo "$line" | awk '{print $2}')
    CMD=$(echo "$line" | awk '{for(i=11;i<=NF;i++) printf "%s ", $i; print ""}')
    PIDS+=("$PID")
    INFOS+=("$CMD")
    echo -e "${GREEN}[$i]${NC} PID: $PID"
    echo "    $CMD" | cut -c1-80
    echo ""
    ((i++))
done <<< "$QEMU_PROCS"

# Ask user to select
echo "------------------------"
echo -n "Select instance to stop (1-$((i-1))) or 'q' to quit: "
read -r choice

if [ "$choice" = "q" ] || [ "$choice" = "Q" ]; then
    info "Cancelled."
    exit 0
fi

# Validate input
if ! [[ "$choice" =~ ^[0-9]+$ ]] || [ "$choice" -lt 1 ] || [ "$choice" -gt $((i-1)) ]; then
    error "Invalid selection."
    exit 1
fi

# Get selected PID
SELECTED_PID=${PIDS[$((choice-1))]}

info "Stopping QEMU instance with PID: $SELECTED_PID..."
kill "$SELECTED_PID" 2>/dev/null

if [ $? -eq 0 ]; then
    success "QEMU instance (PID: $SELECTED_PID) stopped successfully."
else
    error "Failed to stop QEMU instance. Try: kill -9 $SELECTED_PID"
fi

