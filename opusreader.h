#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#define WAVEFORM_SAMPLES_COUNT 64 // governed by WhatsApp, see https://github.com/tulir/whatsmeow/discussions/432 for details

struct opusfile_info {
    int64_t length_seconds;
    char waveform[WAVEFORM_SAMPLES_COUNT];
};

struct opusfile_info opusfile_get_info(void *data, size_t size);

/* Decodes an Ogg/Opus file to a 16-bit PCM WAV file (48 kHz).
 * Returns the duration in seconds, or -1 on error. */
int64_t opusfile_decode_file_to_wav(const char *ogg_path, const char *wav_path);
