#!/bin/bash

DRIVER_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

echo "========================================="
echo "   TASCAM RAW DRIVER LAUNCHER"
echo "========================================="

echo "Select Sample Rate:"
echo "   1) 44100 Hz"
echo "   2) 48000 Hz (Recommended)"
echo "   3) 88200 Hz"
echo "   4) 96000 Hz"
read -p "Enter choice [1-4]: " rate_choice

case $rate_choice in
    1) RATE=44100 ;;
    2) RATE=48000 ;;
    3) RATE=88200 ;;
    4) RATE=96000 ;;
    *) echo "Invalid choice. Defaulting to 48000."; RATE=48000 ;;
esac

echo ""
read -p "Enter Buffer Size (frames) [e.g., 64]: " BUFFER
if ! [[ "$BUFFER" =~ ^[0-9]+$ ]]; then
    echo "Invalid buffer size. Defaulting to 64."
    BUFFER=64
fi

LATENCY=$(awk "BEGIN { printf \"%.2f\", (($BUFFER * 2 * 1000 / $RATE) + 1.0) }")
echo ""
echo "-----------------------------------------"
echo "   Estimated Playback Latency: ${LATENCY} ms"
echo "-----------------------------------------"
echo ""

cleanup() {
    echo ""
    echo "========================================="
    echo "           SHUTTING DOWN"
    echo "========================================="

    if [ ! -z "$CLIENT_PID" ]; then
        echo "[*] Stopping Client..."
        kill $CLIENT_PID 2>/dev/null
        wait $CLIENT_PID 2>/dev/null
    fi

    if [ ! -z "$A2J_PID" ]; then
        echo "[*] Stopping a2jmidid..."
        kill $A2J_PID 2>/dev/null
        wait $A2J_PID 2>/dev/null
    fi

    if [ ! -z "$JACK_PID" ]; then
        echo "[*] Stopping JACK..."
        kill $JACK_PID 2>/dev/null
        wait $JACK_PID 2>/dev/null
    fi

    echo "[*] Unloading Raw Driver..."
    for i in {1..10}; do
        if sudo rmmod tascam_raw 2>/dev/null; then
            break
        fi
        sleep 0.5
    done

    echo "[*] Restoring Standard ALSA Driver..."
    sudo modprobe snd_usb_us144mkii 2>/dev/null

    echo "[*] Restarting PipeWire/PulseAudio..."
    systemctl --user start pipewire.socket pipewire-pulse.socket wireplumber.service 2>/dev/null

    echo "Done. Desktop audio restored."
    exit
}

trap cleanup SIGINT

echo ""
echo "--- INITIALIZING ---"

cd "$DRIVER_DIR"
if [ ! -f "jack_tascam" ] || [ "jack_tascam.c" -nt "jack_tascam" ] || [ ! -f "tascam_raw.ko" ]; then
    echo "[1/6] Compiling..."
    make clean > /dev/null
    make > /dev/null
    if [ $? -ne 0 ]; then echo "Error: Compilation failed."; exit 1; fi
fi

echo "[2/6] Stopping Desktop Audio Services..."
systemctl --user stop pipewire.socket pipewire-pulse.socket wireplumber.service 2>/dev/null
killall -9 jackd 2>/dev/null
killall -9 a2jmidid 2>/dev/null

echo "[3/6] Loading Kernel Driver..."
sudo rmmod snd_usb_us144mkii 2>/dev/null
sudo rmmod snd_usb_audio 2>/dev/null
sudo rmmod tascam_raw 2>/dev/null

TASCAM_BUS_ID=$(grep -l "0644" /sys/bus/usb/devices/*/idVendor | xargs grep -l -E "8020|800f" | sed 's|/idProduct||' | xargs -I{} basename {})
if [ ! -z "$TASCAM_BUS_ID" ]; then
    if [ -e "/sys/bus/usb/devices/$TASCAM_BUS_ID:1.0/driver/unbind" ]; then
        echo "$TASCAM_BUS_ID:1.0" | sudo tee /sys/bus/usb/devices/$TASCAM_BUS_ID:1.0/driver/unbind > /dev/null
    fi
    if [ -e "/sys/bus/usb/devices/$TASCAM_BUS_ID:1.1/driver/unbind" ]; then
        echo "$TASCAM_BUS_ID:1.1" | sudo tee /sys/bus/usb/devices/$TASCAM_BUS_ID:1.1/driver/unbind > /dev/null
    fi
fi

if ! sudo insmod tascam_raw.ko; then
    echo "Error: Failed to insert kernel module."
    cleanup
fi

echo "      Waiting for device node..."
for ((i=0; i<50; i++)); do
    if [ -e /dev/tascam_raw ]; then break; fi
    sleep 0.1
done

if [ ! -e /dev/tascam_raw ]; then
    echo "Error: /dev/tascam_raw was not created."
    sudo dmesg | tail -n 5
    cleanup
fi

sudo chmod 666 /dev/tascam_raw /dev/tascam_midi 2>/dev/null

echo "[4/6] Starting JACK ($RATE Hz / $BUFFER frames)..."
jackd -R -d dummy -r $RATE -p $BUFFER >/dev/null 2>&1 &
JACK_PID=$!
sleep 2

if ! ps -p $JACK_PID > /dev/null; then
    echo "Error: JACK failed to start."
    cleanup
fi

if command -v a2jmidid &> /dev/null; then
    echo "[5/6] Starting a2jmidid..."
    a2jmidid -e >/dev/null 2>&1 &
    A2J_PID=$!
else
    echo "[5/6] a2jmidid not found (skipping MIDI bridge)."
fi

echo "[6/6] Starting TASCAM Bridge Client..."
./jack_tascam &
CLIENT_PID=$!
sleep 1

if ! ps -p $CLIENT_PID > /dev/null; then
    echo "Error: Client failed to start."
    cleanup
fi

echo ""
echo "========================================="
echo "   SYSTEM LIVE!"
echo "   Rate: $RATE Hz | Buffer: $BUFFER"
echo "   Estimated Playback Latency: ${LATENCY} ms"
echo "   Connect your apps in JACK now."
echo "   Press Ctrl+C to stop and restore."
echo "========================================="

wait $CLIENT_PID
