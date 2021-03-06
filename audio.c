#include <complex.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <fftw3.h>
#include "audio.h"
#include "debug.h"
#include "sound_io.h"
#include "ring_buffer.h"
#include <soundio/soundio.h>
#include <string.h>
#include "lua_config.h"

#define CHUNK_SIZE 4096
#define FFT_SIZE (CHUNK_SIZE / 2 + 1)
#define SCALE_RATIO_DOWN 0.05 // Weight put on new maximum when scaling down
#define SCALE_RATIO_UP 1.0 // Weight put on new maximum when scaling up
#define SMOOTH_RATIO_DOWN 0.1 // Weight put on new value when it's less than the old
#define SMOOTH_RATIO_UP 1.0 // Weight put on new value when it's greater than the old

void read_chunk(int16_t *buf, size_t nmemb, struct ring_buffer *rb) {
  rb_read(rb, buf, nmemb * sizeof *buf);
  int8_t *tmp = (int8_t *)buf;
  for (size_t i = 0; i < nmemb; ++i) {
    int8_t *data = tmp + i * sizeof *buf;
    int16_t x = data[0] | data[1] << 8;
    buf[i] = x;
  }
}

struct buffer bars;

// Audio processing state
int nbar;
unsigned scale;
struct ring_buffer *rb;
int nchan;
int16_t *abuf;
double *in;
complex *out;
fftw_plan p;

static inline void bar_calc(float delta) {
  unsigned max = 0;
  unsigned tmp[bars.len];

  for (size_t i = 0; i < bars.len; i++) {
    size_t idx_min = pow(FFT_SIZE, (double)i / bars.len);
    size_t idx_max = pow(FFT_SIZE, (double)(i+1) / bars.len);
    double avg = 0;
    if (idx_min == idx_max) {
      avg = i == 0 ? 0.0f : tmp[i-1];
    } else {
      for (size_t j = idx_min; j < idx_max; j++) {
        avg += cabs(out[j]);
      }
      avg /= idx_max - idx_min;
    }
    if (avg > max) max = avg;
    tmp[i] = avg;
  }

  double x = 0;

  if (max > scale) {
    scale = max * SCALE_RATIO_UP * delta * 60 + scale * (1 - SCALE_RATIO_UP * delta * 60);
  } else if (max < scale) {
    scale = max * SCALE_RATIO_DOWN * delta * 60 + scale * (1 - SCALE_RATIO_DOWN * delta * 60);
  }
  double val_coef = (double)BAR_MAX / scale;

  for (size_t i = 0; i < bars.len; ++i) {
    x = tmp[i] * val_coef;
    if (x > bars.buf[i]) {
      bars.buf[i] = x * SMOOTH_RATIO_UP * delta * 60 + bars.buf[i] * (1 - SMOOTH_RATIO_UP * delta * 60);
    } else if (x < bars.buf[i]) {
      bars.buf[i] = x * SMOOTH_RATIO_DOWN * delta * 60  + bars.buf[i] * (1 - SMOOTH_RATIO_DOWN * delta * 60);
    }
  }
}

void process_audio(float delta) {
  read_chunk(abuf, CHUNK_SIZE * nchan, rb);
  for (size_t i = 0; i < CHUNK_SIZE; ++i) {
    in[i] = 0;
    for (size_t j = 0; j < nchan; ++j)
      in[i] += abuf[i * nchan + j];
    in[i] /= nchan;
  }
  fftw_execute(p);
  bar_calc(delta);
}

int audio_init(struct config *config) {
  nbar = config->bars;
  rb = config->sinfo.instream->userdata;

  scale = INT16_MAX;

  nchan = config->sinfo.instream->layout.channel_count;
  abuf = malloc(nchan * CHUNK_SIZE * sizeof *abuf);

  in = fftw_malloc(CHUNK_SIZE * sizeof *in);
  if (!in) return -1;
  out = fftw_alloc_complex(FFT_SIZE);
  if (!out) {
    fftw_free(in);
    return -1;
  }

  DEBUG("Initialising FFT plan...");
  p = fftw_plan_dft_r2c_1d(CHUNK_SIZE, in, out, FFTW_MEASURE);
  DEBUG("FFT init done");

  bars.buf = calloc(nbar, sizeof *bars.buf);
  if (!bars.buf) {
    fftw_free(in);
    fftw_free(out);
    return -1;
  }
  bars.len = nbar;
  return 0;
}

