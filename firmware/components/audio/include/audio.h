/*
 * Audio in and out: the INMP441 microphone on I2S controller 0 and the
 * MAX98357A amplifier on I2S controller 1.
 *
 * Both directions carry 16-bit signed mono samples. Capture always runs at
 * AUDIO_CAPTURE_RATE; playback runs at whatever rate each playback asks for,
 * because the server announces the rate of each reply.
 *
 * Timeouts are in milliseconds, the unit the I2S driver itself takes.
 *
 * Threading: each direction has one user at a time. Capture functions must
 * not be called from two tasks at once, nor playback functions, but capture
 * and playback may run concurrently in different tasks.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Samples per second delivered by the microphone. 16 kHz is what both the
 * wake-word model and the speech-to-text service expect. */
#define AUDIO_CAPTURE_RATE  16000

/* The CPU core reserved for audio work (ARCHITECTURE.md, task and core
 * allocation). audio_init() sets up the I2S interrupts on this core, and tasks
 * that move audio samples should be pinned to it, away from the WiFi
 * interrupts on core 0. */
#define AUDIO_CORE          1

/* Sets up both I2S controllers, starts the microphone, and drives the
 * amplifier's SD pin low so it stays off until something plays. Call once,
 * early in boot, from any core: the set-up itself runs on core 1, so the I2S
 * interrupts do too. Every other function returns ESP_ERR_INVALID_STATE until
 * this has succeeded.
 *
 * The microphone then runs continuously. Its output takes about 2 s to settle
 * after this call, so audio from the first seconds after boot is not usable. */
esp_err_t audio_init(void);

/* Discards audio buffered while nobody was reading, up to about 75 ms of it,
 * so the next read starts with sound from now. Returns without waiting. */
esp_err_t audio_capture_flush(void);

/* Fills `out` with exactly `frames` samples, blocking until they have been
 * recorded. Reads continue where the previous read stopped, so a caller
 * reading in a loop gets an unbroken stream, provided no more than about
 * 75 ms passes between reads; beyond that, the oldest audio is lost. Returns
 * an error, usually ESP_ERR_TIMEOUT, if the microphone delivers nothing for
 * `timeout_ms`. */
esp_err_t audio_capture_read(int16_t *out, size_t frames, uint32_t timeout_ms);

/* Starts the amplifier at `sample_rate` samples per second (8000-48000;
 * ESP_ERR_INVALID_ARG otherwise), playing silence
 * until audio_play_write() supplies samples. Returns once the amplifier is
 * ready to play, about 20 ms after the call. */
esp_err_t audio_play_start(uint32_t sample_rate);

/* Queues `frames` samples for playback, blocking until all are queued.
 * Playback is paced by the hardware, so a caller writing in a loop runs in
 * real time. Returns ESP_ERR_TIMEOUT if there was no room for `timeout_ms`. */
esp_err_t audio_play_write(const int16_t *samples, size_t frames, uint32_t timeout_ms);

/* Waits for queued audio to finish playing, then turns the amplifier off.
 * Blocks for up to about 105 ms at 16 kHz, 70 ms at 24 kHz. */
esp_err_t audio_play_stop(void);
