#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <jack/jack.h>
#include <jack/midiport.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <signal.h>
#include "tascam_common.h"

#define DEVICE_NODE "/dev/tascam_raw"
#define MIDI_NODE "/dev/tascam_midi"
#define CHANNELS 4
#define BYTES_PER_SAMPLE 3
#define S24_MAX 8388607.0
#define RAW_BLOCK_SIZE 512
#define FRAMES_PER_BLOCK 8

int fd, midi_fd;
volatile sig_atomic_t keep_running = 1;

jack_client_t *client_pb;
jack_client_t *client_cap;
jack_client_t *client_midi;

jack_port_t *pb_ports[CHANNELS];
jack_port_t *cap_ports[CHANNELS];
jack_port_t *midi_in_port;
jack_port_t *midi_out_port;

const char *pb_port_names[] = {
    "Analog Left", "Analog Right",
    "Digital Left", "Digital Right"
};

const char *cap_port_names[] = {
    "Analog Left", "Analog Right",
    "Digital Left", "Digital Right"
};

void signal_handler(int sig) {
    keep_running = 0;
}

void decode_block(const uint8_t *src, float **dst_ch, int offset) {
    int frame, bit;
    for (frame = 0; frame < FRAMES_PER_BLOCK; ++frame) {
        const uint8_t *p_src = src + frame * 64;
        int32_t ch[4] = {0};
        for (bit = 0; bit < 24; ++bit) {
            uint8_t byte1 = p_src[bit];
            ch[0] = (ch[0] << 1) | (byte1 & 1);
            ch[2] = (ch[2] << 1) | ((byte1 >> 1) & 1);
        }
        for (bit = 0; bit < 24; ++bit) {
            uint8_t byte2 = p_src[bit + 32];
            ch[1] = (ch[1] << 1) | (byte2 & 1);
            ch[3] = (ch[3] << 1) | ((byte2 >> 1) & 1);
        }
        for(int c=0; c<4; c++) {
            int32_t val = ch[c];
            if (val & 0x800000) val |= 0xFF000000;
            dst_ch[c][offset + frame] = (float)val / S24_MAX;
        }
    }
}

int process_pb(jack_nframes_t nframes, void *arg) {
    jack_default_audio_sample_t *pb_in[CHANNELS];

    for (int i = 0; i < CHANNELS; i++) {
        pb_in[i] = (jack_default_audio_sample_t *)jack_port_get_buffer(pb_ports[i], nframes);
    }

    size_t pb_bytes = nframes * CHANNELS * BYTES_PER_SAMPLE;
    unsigned char *pb_buf = malloc(pb_bytes);
    if (!pb_buf) return 0;

    unsigned char *ptr = pb_buf;
    for (int i = 0; i < nframes; i++) {
        for (int c = 0; c < CHANNELS; c++) {
            float s = pb_in[c][i];
            if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
            int32_t val = (int32_t)(s * S24_MAX);
            *ptr++ = (val >> 0) & 0xFF;
            *ptr++ = (val >> 8) & 0xFF;
            *ptr++ = (val >> 16) & 0xFF;
        }
    }

    if (write(fd, pb_buf, pb_bytes)) {};
    free(pb_buf);
    return 0;
}

int process_cap(jack_nframes_t nframes, void *arg) {
    jack_default_audio_sample_t *cap_out[CHANNELS];
    for (int i = 0; i < CHANNELS; i++) {
        cap_out[i] = (jack_default_audio_sample_t *)jack_port_get_buffer(cap_ports[i], nframes);
    }

    int blocks_needed = nframes / FRAMES_PER_BLOCK;
    if (nframes % FRAMES_PER_BLOCK != 0) blocks_needed++;

    size_t raw_bytes = blocks_needed * RAW_BLOCK_SIZE;
    unsigned char *raw_buf = malloc(raw_bytes);
    if (!raw_buf) return 0;

    ssize_t read_len = read(fd, raw_buf, raw_bytes);

    float *dst_ptrs[4] = { cap_out[0], cap_out[1], cap_out[2], cap_out[3] };
    if (read_len > 0) {
        int blocks_read = read_len / RAW_BLOCK_SIZE;
        for(int b=0; b<blocks_read; b++) {
            if ((b * FRAMES_PER_BLOCK) < nframes) {
                decode_block(raw_buf + (b * RAW_BLOCK_SIZE), dst_ptrs, b * FRAMES_PER_BLOCK);
            }
        }
    } else {
        for(int c=0; c<4; c++) memset(cap_out[c], 0, nframes * sizeof(float));
    }
    free(raw_buf);
    return 0;
}

int process_midi(jack_nframes_t nframes, void *arg) {
    void *midi_in_buf = jack_port_get_buffer(midi_in_port, nframes);
    void *midi_out_buf = jack_port_get_buffer(midi_out_port, nframes);
    jack_midi_clear_buffer(midi_out_buf);

    jack_nframes_t event_count = jack_midi_get_event_count(midi_in_buf);
    jack_midi_event_t event;
    for(int i=0; i<event_count; i++) {
        jack_midi_event_get(&event, midi_in_buf, i);
        if (event.size > 8) continue;
        uint8_t packet[9];
        memcpy(packet, event.buffer, event.size);
        if (event.size < 8) memset(packet + event.size, 0xFD, 8 - event.size);
        packet[8] = 0xE0;
        if (write(midi_fd, packet, 9)) {};
    }

    uint8_t raw_midi[9 * 16];
    ssize_t m_read = read(midi_fd, raw_midi, sizeof(raw_midi));
    if (m_read > 0) {
        int packets = m_read / 9;
        for(int p=0; p<packets; p++) {
            uint8_t *pkt = raw_midi + (p*9);
            int len = 0;
            while(len < 8 && pkt[len] != 0xFD) len++;
            if (len > 0) jack_midi_event_write(midi_out_buf, 0, pkt, len);
        }
    }
    return 0;
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    fd = open(DEVICE_NODE, O_RDWR);
    if (fd < 0) { fprintf(stderr, "Could not open %s\n", DEVICE_NODE); return 1; }

    midi_fd = open(MIDI_NODE, O_RDWR | O_NONBLOCK);
    if (midi_fd < 0) { fprintf(stderr, "Could not open %s\n", MIDI_NODE); return 1; }

    client_pb = jack_client_open("TASCAM Output", JackNullOption, NULL);
    if (!client_pb) { fprintf(stderr, "Failed to create Playback client\n"); return 1; }

    int rate = jack_get_sample_rate(client_pb);
    printf("JACK Sample Rate: %d Hz\n", rate);
    if (ioctl(fd, TASCAM_IOC_SET_RATE, &rate) < 0) { perror("IOCTL failed"); return 1; }

    jack_set_process_callback(client_pb, process_pb, NULL);
    for (int i = 0; i < CHANNELS; i++) {
        pb_ports[i] = jack_port_register(client_pb, pb_port_names[i], JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput | JackPortIsTerminal, 0);
    }

    client_cap = jack_client_open("TASCAM Input", JackNullOption, NULL);
    if (!client_cap) { fprintf(stderr, "Failed to create Capture client\n"); return 1; }

    jack_set_process_callback(client_cap, process_cap, NULL);
    for (int i = 0; i < CHANNELS; i++) {
        cap_ports[i] = jack_port_register(client_cap, cap_port_names[i], JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput | JackPortIsTerminal, 0);
    }

    client_midi = jack_client_open("TASCAM MIDI", JackNullOption, NULL);
    if (!client_midi) { fprintf(stderr, "Failed to create MIDI client\n"); return 1; }

    jack_set_process_callback(client_midi, process_midi, NULL);
    midi_in_port = jack_port_register(client_midi, "MIDI OUT", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput | JackPortIsTerminal, 0);
    midi_out_port = jack_port_register(client_midi, "MIDI IN", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput | JackPortIsTerminal, 0);

    if (jack_activate(client_pb)) return 1;
    if (jack_activate(client_cap)) return 1;
    if (jack_activate(client_midi)) return 1;

    printf("TASCAM Raw JACK Clients Running.\n");
    printf("  [1] TASCAM Output (Playback)\n");
    printf("  [2] TASCAM Input  (Capture)\n");
    printf("  [3] TASCAM MIDI   (MIDI)\n");
    printf("Running in background. Send SIGINT (Ctrl+C) to stop.\n");

    while(keep_running) {
        sleep(1);
    }

    printf("\nStopping...\n");
    jack_client_close(client_pb);
    jack_client_close(client_cap);
    jack_client_close(client_midi);
    close(fd);
    close(midi_fd);
    return 0;
}
