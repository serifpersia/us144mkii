#!/bin/bash

# MIT License
# Copyright (c) 2025 serifpersia
#
# Interactive launcher for the TASCAM US-144MKII FIFO streamer.
# Prompts for sample rate, latency profile, and logging options,
# then configures PulseAudio and the C streamer binary accordingly.

# --- Configuration ---
SINK_NAME="TASCAM-US144MKII-OUT"
FIFO_PLAYBACK_PATH="/tmp/tascam-audio-playback"
STREAMER_BINARY="./tascam_streamer" # Assumes the C program is in the same directory
CHANNELS="2"
FORMAT="s24le"

# --- Cleanup Function ---
cleanup() {
    echo ""
    echo "--- Running cleanup... ---"
    
    pkill -f "$STREAMER_BINARY" 2>/dev/null
    sleep 0.5

    echo "Unloading PulseAudio module..."
    pactl unload-module module-pipe-sink 2>/dev/null
    
    echo "Removing FIFO file..."
    rm -f "$FIFO_PLAYBACK_PATH"
    
    echo "--- Cleanup complete. ---"
    exit 0
}

# Trap signals to ensure cleanup runs
trap cleanup SIGINT TERM EXIT

# --- Interactive Setup ---
echo "--- TASCAM Streamer Interactive Setup ---"

# 1. Select Sample Rate
rates=("44100" "48000" "88200" "96000")
PS3="Please select a sample rate: "
select rate_choice in "${rates[@]}"; do
    if [[ -n "$rate_choice" ]]; then
        SELECTED_RATE="$rate_choice"
        echo "Selected rate: $SELECTED_RATE Hz"
        break
    else
        echo "Invalid selection. Please try again."
    fi
done
echo ""

# 2. Select Latency Profile
profiles=("0: Lowest" "1: Low" "2: Normal" "3: High" "4: Highest")
PS3="Please select a latency profile: "
select profile_choice in "${profiles[@]}"; do
    if [[ -n "$profile_choice" ]]; then
        SELECTED_PROFILE_INDEX=$((REPLY - 1))
        echo "Selected profile: $profile_choice"
        break
    else
        echo "Invalid selection. Please try again."
    fi
done
echo ""

# 3. Select Logging Mode
LOG_MODE_FLAG=""
LOG_INTERVAL_FLAG=""
read -p "Use minimal logging instead of the live dashboard? (y/n) [default: n]: " minimal_choice
if [[ "$minimal_choice" == "y" || "$minimal_choice" == "Y" ]]; then
    LOG_MODE_FLAG="--minimal-log"
    read -p "Enter log interval in milliseconds [default: 1000]: " interval_ms
    if [[ -z "$interval_ms" ]]; then
        interval_ms=1000 # Set default if user enters nothing
    fi
    LOG_INTERVAL_FLAG="--log-interval $interval_ms"
    LOG_MODE_SUMMARY="Minimal (updates every ${interval_ms}ms)"
else
    LOG_MODE_SUMMARY="Live Dashboard (updates every 100ms)"
fi

echo "---------------------------------------------"
echo "Configuration:"
echo "  Rate:    $SELECTED_RATE Hz"
echo "  Profile: $SELECTED_PROFILE_INDEX ($profile_choice)"
echo "  Logging: $LOG_MODE_SUMMARY"
echo "---------------------------------------------"

# --- Main Execution ---
rm -f "$FIFO_PLAYBACK_PATH"
echo "Creating playback FIFO at $FIFO_PLAYBACK_PATH..."
mkfifo "$FIFO_PLAYBACK_PATH"

echo "Loading PulseAudio pipe-sink module..."
SINK_MODULE_ID=$(pactl load-module module-pipe-sink file="$FIFO_PLAYBACK_PATH" sink_name="$SINK_NAME" format=$FORMAT rate=$SELECTED_RATE channels=$CHANNELS)
if [ -z "$SINK_MODULE_ID" ]; then
    echo "Error: Failed to load PulseAudio pipe-sink module. Aborting."
    exit 1
fi
echo "Playback Sink ('$SINK_NAME') loaded with ID: $SINK_MODULE_ID"
echo "You can now select '$SINK_NAME' as an output device in your sound settings."
echo "---------------------------------------------"

echo "Starting C streamer binary..."
# Launch the C program with all selected arguments.
# The log flags will be empty strings if not selected, which bash ignores.
sudo "$STREAMER_BINARY" \
    -r "$SELECTED_RATE" \
    -p "$SELECTED_PROFILE_INDEX" \
    --pipe "$FIFO_PLAYBACK_PATH" \
    $LOG_MODE_FLAG \
    $LOG_INTERVAL_FLAG

echo "Streamer exited. Waiting for cleanup..."
wait
