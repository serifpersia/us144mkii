# AI Coding Agent Instructions for TASCAM US-144MKII Driver

## Project Overview
ALSA kernel driver for TASCAM US-144MKII USB audio interface. Implements PCM playback/capture and MIDI I/O via USB isochronous transfers. Device-specific configuration includes sample rate setup, feedback synchronization, and multi-device support (US-144, US-144MKII, US-122MKII variants).

## Architecture

### Core Components
- **us144mkii.c/h**: Main driver module, device probing, USB lifecycle management, struct `tascam_card` (central device context)
- **us144mkii_playback.c**: PCM playback callbacks, URB (USB Request Block) submission, implicit feedback handling
- **us144mkii_capture.c**: PCM capture callbacks, isochronous frame processing, timestamp calculation
- **us144mkii_midi.c**: MIDI input/output via separate URBs, packet formatting (9-byte packets with 8-byte payload)
- **us144mkii_pcm.c/h**: Device configuration, sample rate switching, USB control messages

### Data Flow Pattern
1. User app → ALSA → `playback_trigger()` / `capture_trigger()` → URB submission
2. USB device ↔ `playback_urb_complete()` / `capture_urb_complete()` callbacks
3. Callbacks update `driver_playback_pos` / `driver_capture_pos` and notify via `snd_pcm_period_elapsed()`
4. MIDI: separate endpoint-based URB chain, packet format defined in `tascam_midi_output_trigger()`

### Key Design Decisions
- **Feedback synchronization**: Playback uses implicit feedback (capture endpoint provides frame counts) to sync with device clock. See `feedback_urb_complete()` for PLL filter logic (`phase_accum`, `freq_q16`).
- **Device variants**: Conditional logic on `tascam->dev_id` handles differences (US-122MKII uses UAC control messages; US-144MKII requires vendor register writes). See `us144mkii_configure_device_for_rate()`.
- **URB anchors**: `usb_anchor` structures manage URB lifecycle (kill on stop, avoid use-after-free).
- **Spin locks**: Protect shared state (`playback_active`, `capture_active`), especially in interrupt context callbacks.

## Build & Development Workflow

### Build
```bash
make clean && make
# Generates: snd-usb-us144mkii.ko (combined module from 5 .o files)
```

### Install & Test
```bash
./build_and_install.sh
# Steps: clean → make → copy to /lib/modules/*/extra → depmod → modprobe
```

### Debugging
- Monitor module logs: `dmesg | tail -f` (check `dev_err()` in code)
- List loaded module: `lsmod | grep us144mkii`
- Inspect device: `lsusb -d 0644:8020` (TASCAM vendor:device ID)
- Unload: `sudo rmmod snd_usb_us144mkii`
- Reload: `sudo insmod ./snd-usb-us144mkii.ko`

## Critical Patterns

### USB Control Messages
Rate configuration uses **vendor-specific** vs **UAC** requests depending on device:
```c
// US-144MKII: vendor register writes (3 register addresses per rate)
usb_control_msg(dev, usb_sndctrlpipe(dev, 0), 
                VENDOR_REQ_REGISTER_WRITE, RT_H2D_VENDOR_DEV, ...)

// US-122MKII: standard UAC sampling frequency control
usb_control_msg(dev, usb_sndctrlpipe(dev, 0), 
                UAC_SET_CUR, RT_H2D_CLASS_EP, ...)
```

### URB Completion Callbacks
Always run with spinlock for `midi_lock` and driver `lock`. Use `spin_lock_irqsave()` when called from both user/interrupt context.

### Memory: USB DMA Coherency
Allocate transfer buffers with `usb_alloc_coherent()`, not `kmalloc()`. See `tascam_alloc_urbs()` for allocation patterns. **Never** stack-allocate or use non-DMA buffers for `urb->transfer_buffer`.

### Period Position Tracking
- Playback: `playback_frames_consumed` incremented in `playback_urb_complete()` per URB
- Capture: `capture_frames_processed` incremented per valid sample in `capture_urb_complete()`
- Return modulo `buffer_size` in pointer callbacks for ALSA period interrupt calculation

## Supported Sample Rates & Formats
- **Rates**: 44.1 kHz, 48 kHz, 88.2 kHz, 96 kHz (configured via `us144mkii_configure_device_for_rate()`)
- **Playback**: S24_3LE (24-bit signed, 3 bytes/sample), 4 channels, USB → payload packing
- **Capture**: S32_LE (32-bit signed, native USB format), 4 channels (2 for US-122MKII)
- **MIDI**: 8 payload bytes per packet + 1 status byte (0xE0)

## Integration & External Dependencies
- **ALSA**: `snd_card_new()`, `snd_pcm_new()`, `snd_rawmidi_new()` for device registration
- **USB**: `usb_register()` module, probe/disconnect lifecycle, urb/endpoint descriptors
- **Kernel**: GPL-2.0-only licensed, compatible with mainline (merged into `sound/for-next` branch)

## Convention Notes
- Hex values (USB IDs, register addresses) use `0x` prefix consistently
- Payload arrays are `static const u8[]` for rate-specific values
- Use `ARRAY_SIZE()` macro for register write loops
- Error returns are negative (`-ENOMEM`, `-EINVAL`, `-EIO`)

## When Adding Features
1. Verify device variant (check `tascam->dev_id` conditionals)
2. If new USB control needed: add to `us144mkii_configure_device_for_rate()` or probe
3. If new URB callback: allocate in `tascam_alloc_urbs()`, free in `tascam_free_urbs()`, anchor in appropriate list
4. Rate changes: ensure both control messages + register writes executed (see rate payload logic)
5. MIDI: remember 9-byte packet format (8 payload + 0xE0 status)
