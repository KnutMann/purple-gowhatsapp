#include "opusreader.h"

#if __has_include("opusfile.h")

#include <opusfile.h>
#include <glib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

// TODO: assert ogg_int64_t is int64_t because that is what cgo is written to expect

/**
 * Generates a "waveform" consisting of WAVEFORM_SAMPLES_COUNT values of average amplitude over time.
 * 
 * Assumes a one channel input.
 */
static void opusfile_make_waveform(OggOpusFile * of, ogg_int64_t samples_count, char *waveform) {
  float waveform_float[WAVEFORM_SAMPLES_COUNT] = {};
  ogg_int64_t samples_per_waveform_bin = samples_count/WAVEFORM_SAMPLES_COUNT;
  ogg_int64_t total_samples_position = 0;
  float pcm[120*48]; // minimum buffer size according to https://opus-codec.org/docs/opusfile_api-0.7/group__stream__decoding.html
  int samples_read_count = op_read_float(of, pcm, samples_count, NULL); 

  // read samples to sum up sample values into waveform bins
  while (samples_read_count > 0) {
    for (int sample_index = 0; sample_index < samples_read_count; sample_index++) {
      ogg_int64_t waveform_index = total_samples_position/samples_per_waveform_bin; // note: this may drop some of the last samples
      waveform_float[waveform_index] += fabs(pcm[sample_index]);
      total_samples_position++;
    }
    // next iteration
    samples_read_count = op_read_float(of, pcm, samples_count, NULL);
  }
  
  // find maximum for normalization
  float max = 0.0f;
  for (int waveform_index = 0; waveform_index < WAVEFORM_SAMPLES_COUNT; waveform_index++) {
    if (max < waveform_float[waveform_index]) {
      max = waveform_float[waveform_index];
    }
  }
  
  // normalize and convert to percent
  for (int waveform_index = 0; waveform_index < WAVEFORM_SAMPLES_COUNT; waveform_index++) {
    waveform[waveform_index] = (char)(waveform_float[waveform_index]/max*100);
  }
}

struct opusfile_info opusfile_get_info(void *data, size_t size) {
  struct opusfile_info info = {.length_seconds = -1}; // use negative length to indicate error
  int error;
  OggOpusFile * of = op_open_memory(data, size, &error); // MEMCHECK: released here
  if (of != NULL) {
    if (op_channel_count(of, -1) == 1) { // only one-channel recordings are supported
      ogg_int64_t samples_count = op_pcm_total(of, -1);
      info.length_seconds = samples_count/48000;
      opusfile_make_waveform(of, samples_count, info.waveform);
    } else {
      //printf("op_channel_count returned %d instead of 1.\n", op_channel_count(of, -1));
    }
    op_free(of);
  } else {
    //printf("op_open_memory returned NULL. error is %d.\n", error);
  }
  return info;
}

int64_t opusfile_decode_file_to_wav(const char *ogg_path, const char *wav_path) {
  int error;
  OggOpusFile *of = op_open_file(ogg_path, &error);
  if (of == NULL) {
    return -1;
  }
  int channels = op_channel_count(of, -1);
  if (channels < 1 || channels > 2) {
    op_free(of);
    return -1;
  }
  FILE *out = fopen(wav_path, "wb");
  if (out == NULL) {
    op_free(of);
    return -1;
  }
  unsigned char header[44] = {0};
  fwrite(header, 1, sizeof(header), out); // placeholder, rewritten below

  opus_int16 pcm[120 * 48 * 2];
  int64_t total_samples = 0;
  int n;
  while ((n = op_read(of, pcm, 120 * 48 * 2, NULL)) > 0) {
    fwrite(pcm, sizeof(opus_int16) * channels, n, out);
    total_samples += n;
  }
  op_free(of);

  uint32_t sample_rate = 48000;
  uint32_t data_bytes = (uint32_t)(total_samples * channels * 2);
  uint32_t byte_rate = sample_rate * channels * 2;
  uint16_t block_align = channels * 2;
  uint32_t riff_size = 36 + data_bytes;
  memcpy(header, "RIFF", 4);
  memcpy(header + 4, &riff_size, 4);
  memcpy(header + 8, "WAVEfmt ", 8);
  uint32_t fmt_size = 16; memcpy(header + 16, &fmt_size, 4);
  uint16_t fmt_pcm = 1; memcpy(header + 20, &fmt_pcm, 2);
  uint16_t ch16 = (uint16_t)channels; memcpy(header + 22, &ch16, 2);
  memcpy(header + 24, &sample_rate, 4);
  memcpy(header + 28, &byte_rate, 4);
  memcpy(header + 32, &block_align, 2);
  uint16_t bits = 16; memcpy(header + 34, &bits, 2);
  memcpy(header + 36, "data", 4);
  memcpy(header + 40, &data_bytes, 4);
  fseek(out, 0, SEEK_SET);
  fwrite(header, 1, sizeof(header), out);
  fclose(out);

  if (total_samples == 0) {
    return -1;
  }
  return total_samples / sample_rate;
}

#else

#pragma message "Warning: Building without opusfile. Sending voice messages is disabled."
struct opusfile_info opusfile_get_info(void *data, size_t size) {
  struct opusfile_info info = {.length_seconds = -1}; // use negative length to indicate error
  return info;
}

int64_t opusfile_decode_file_to_wav(const char *ogg_path, const char *wav_path) {
  return -1;
}

#endif
