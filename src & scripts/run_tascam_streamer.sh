
# MIT License Copyright (c) 2025 serifpersia

#!/bin/bash

# --- Configuration ---
SINK_NAME="TASCAM-US144MKII-OUT"
SOURCE_NAME="TASCAM-US144MKII-IN"
FIFO_PLAYBACK_PATH="/tmp/tascam-audio-playback"
FIFO_CAPTURE_PATH="/tmp/tascam-audio-capture"
RATE="48000"
CHANNELS="2"
STREAMER_BINARY="./tascam_streamer"

# Set format for PulseAudio modules. Playback is s24le.
# Capture is left as s24le but will not be used by the C program.
FORMAT="s24le"

# --- Cleanup Function ---
cleanup() {
    echo ""
    echo "--- Running cleanup... ---"
    
    pkill -f "$STREAMER_BINARY" 2>/dev/null
    sleep 0.5

    echo "Unloading PulseAudio modules..."
    pactl unload-module module-pipe-sink 2>/dev/null
    pactl unload-module module-pipe-source 2>/dev/null
    
    echo "Removing FIFO files..."
    rm -f "$FIFO_PLAYBACK_PATH"
    rm -f "$FIFO_CAPTURE_PATH"
    
    echo "--- Cleanup complete. ---"
    exit 0
}

# Trap signals to ensure cleanup runs
trap cleanup SIGINT TERM EXIT

echo "--- Starting TASCAM Streamer Automation (PLAYBACK ONLY) ---"

rm -f "$FIFO_PLAYBACK_PATH"
rm -f "$FIFO_CAPTURE_PATH"
echo "Creating playback FIFO at $FIFO_PLAYBACK_PATH..."
mkfifo "$FIFO_PLAYBACK_PATH"
echo "Creating (unused) capture FIFO at $FIFO_CAPTURE_PATH..."
mkfifo "$FIFO_CAPTURE_PATH"

echo "Loading PulseAudio pipe-sink module for playback (Format: $FORMAT)..."
SINK_MODULE_ID=$(pactl load-module module-pipe-sink file="$FIFO_PLAYBACK_PATH" sink_name="$SINK_NAME" format=$FORMAT rate=$RATE channels=$CHANNELS)
if [ -z "$SINK_MODULE_ID" ]; then
    echo "Error: Failed to load PulseAudio pipe-sink module. Aborting."
    exit 1
fi
echo "Playback Sink ('$SINK_NAME') loaded with ID: $SINK_MODULE_ID"

echo "Loading (unused) PulseAudio pipe-source module for capture (Format: $FORMAT)..."
SOURCE_MODULE_ID=$(pactl load-module module-pipe-source file="$FIFO_CAPTURE_PATH" source_name="$SOURCE_NAME" format=$FORMAT rate=$RATE channels=$CHANNELS)
if [ -z "$SOURCE_MODULE_ID" ]; then
    echo "Error: Failed to load PulseAudio pipe-source module. Aborting."
    exit 1
fi
echo "Capture Source ('$SOURCE_NAME') loaded with ID: $SOURCE_MODULE_ID"
echo "---------------------------------------------"

echo "Starting C streamer binary for PLAYBACK ONLY..."
sudo "$STREAMER_BINARY" \
    -r "$RATE" \
    --playback-pipe "$FIFO_PLAYBACK_PATH"

echo "Streamer exited. Waiting for cleanup..."
wait
