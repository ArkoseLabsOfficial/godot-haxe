/**************************************************************************/
/*  haxe_audio_stream.cpp                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "haxe_audio_stream.h"

#include "core/math/math_funcs.h"

namespace {

/* A power of two at least as large as `p_v`, so the ring index is a mask. */
int _next_power_of_two(int p_v) {
	int v = 1;
	while (v < p_v) {
		v <<= 1;
	}
	return v;
}

} // namespace

HaxeAudioStream::HaxeAudioStream() {
	// Armed from the start: the queue is empty, so the consumer holds silence
	// until the first backlog exists (and again after every flush).
	warming.set(1);
	_resize_ring();
}

HaxeAudioStream::~HaxeAudioStream() {
}

void HaxeAudioStream::configure(double p_mix_rate, double p_buffer_len, double p_target_len) {
	if (p_mix_rate > 0.0) {
		mix_rate = (float)p_mix_rate;
	}
	if (p_buffer_len > 0.0) {
		buffer_len = (float)p_buffer_len;
	}
	if (p_target_len > 0.0) {
		target_frames = (int)Math::ceil(p_mix_rate * p_target_len);
	}
	_resize_ring();
}

void HaxeAudioStream::_resize_ring() {
	const int wanted = (int)Math::ceil(mix_rate * buffer_len);
	const int capacity = _next_power_of_two(MAX(2048, wanted));

	ring.resize(capacity);
	ring_mask = (uint32_t)(capacity - 1);

	for (int i = 0; i < capacity; i++) {
		ring.write[i] = AudioFrame(0.0f, 0.0f);
	}

	/* Keep the backlog below a quarter of the ring and above a reasonable
	 * minimum, so a target larger than the ring (or a ring too small to hold
	 * it) cannot make the consumer wait for frames that can never queue up. */
	target_frames = CLAMP(target_frames, 256, capacity / 4);

	/* Only a read-ahead that does not fit the ring ever reaches here; see the
	 * header. */
	high_water_frames = capacity - capacity / 8;
}

int HaxeAudioStream::write_frames(const int16_t *p_samples, int p_frames, int p_channels) {
	if (p_samples == nullptr || p_frames <= 0 || p_channels <= 0 || ring.is_empty()) {
		return 0;
	}

	const uint64_t write = write_frame.get();
	const uint64_t read = read_frame.get();
	const uint64_t capacity = (uint64_t)ring.size();
	const uint64_t used = write - read;

	if (used >= capacity) {
		dropped_frames.add((uint64_t)p_frames);
		return 0;
	}

	const uint64_t space = capacity - used;
	const int take = (uint64_t)p_frames < space ? p_frames : (int)space;

	AudioFrame *dst = ring.ptrw();

	for (int i = 0; i < take; i++) {
		// Interleaved signed 16 bit, normalized to [-1, 1]; a mono source is
		// duplicated into both channels.
		const int16_t left = p_samples[(size_t)i * p_channels];
		const int16_t right = p_channels > 1 ? p_samples[(size_t)i * p_channels + 1] : left;
		dst[(write + (uint64_t)i) & ring_mask] = AudioFrame(left / 32768.0f, right / 32768.0f);
	}

	// The samples are written before the count is published, so the consumer
	// can never observe a frame that has not been filled in yet.
	write_frame.set(write + (uint64_t)take);

	if (take < p_frames) {
		dropped_frames.add((uint64_t)(p_frames - take));
	}

	return take;
}

int64_t HaxeAudioStream::buffered_frames() const {
	const uint64_t write = write_frame.get();
	const uint64_t read = read_frame.get();
	return write > read ? (int64_t)(write - read) : 0;
}

int HaxeAudioStream::pull_frames(AudioFrame *p_buffer, int p_frames) {
	if (p_buffer == nullptr || p_frames <= 0) {
		return 0;
	}

	uint64_t write = write_frame.get();
	uint64_t read = read_frame.get();

	if (read > write) {
		// Only reachable if the stream was reset behind the consumer's back;
		// resynchronise instead of reading forever.
		read = write;
		read_frame.set(read);
	}

	uint64_t used = write - read;

	if (used > (uint64_t)high_water_frames) {
		// The read-ahead does not fit the ring, so the sound would trail the
		// picture for good. Skip the oldest frames instead; this is a last
		// resort and does not run for a read-ahead the ring can hold.
		const uint64_t trim = used - (uint64_t)target_frames;
		read += trim;
		used -= trim;
		read_frame.set(read);
		trimmed_frames.add(trim);
	}

	if (warming.get() != 0) {
		if (used < (uint64_t)target_frames) {
			// Hold silence until a small backlog exists, so playback does not
			// start by immediately underrunning on every decoder burst.
			for (int i = 0; i < p_frames; i++) {
				p_buffer[i] = AudioFrame(0.0f, 0.0f);
			}
			return p_frames;
		}
		warming.set(0);
	}

	const int take = used < (uint64_t)p_frames ? (int)used : p_frames;

	const AudioFrame *src = ring.ptr();

	for (int i = 0; i < take; i++) {
		p_buffer[i] = src[(read + (uint64_t)i) & ring_mask];
	}

	read_frame.set(read + (uint64_t)take);

	if (take < p_frames) {
		for (int i = take; i < p_frames; i++) {
			p_buffer[i] = AudioFrame(0.0f, 0.0f);
		}
		underrun_count.increment();
	}

	return p_frames;
}

void HaxeAudioStream::reset_queue() {
	// The producer may publish a frame between these two reads; the consumer
	// then keeps at most that one, which is harmless. Discarding it exactly
	// would need the producer to stop, which would stall a decoder thread.
	read_frame.set(write_frame.get());
	warming.set(1);
}

Ref<AudioStreamPlayback> HaxeAudioStream::instantiate_playback() {
	Ref<HaxeAudioStreamPlayback> playback;
	playback.instantiate();
	playback->stream = Ref<HaxeAudioStream>(this);
	return playback;
}

String HaxeAudioStream::get_stream_name() const {
	return "HaxeAudioStream";
}

double HaxeAudioStream::get_length() const {
	return 0;
}

bool HaxeAudioStream::is_monophonic() const {
	return true;
}

/* --------------------------------------------------------------- playback -- */

HaxeAudioStreamPlayback::HaxeAudioStreamPlayback() {
}

int HaxeAudioStreamPlayback::_mix_internal(AudioFrame *p_buffer, int p_frames) {
	if (p_buffer == nullptr || p_frames <= 0) {
		return 0;
	}

	if (!active || stream.is_null()) {
		// A short count tells the audio server the stream is over, so silence
		// for an inactive playback is reported as no frames at all, exactly
		// like `AudioStreamGeneratorPlayback`.
		return 0;
	}

	return stream->pull_frames(p_buffer, p_frames);
}

float HaxeAudioStreamPlayback::get_stream_sampling_rate() {
	return stream.is_valid() ? stream->mix_rate : 44100.0f;
}

void HaxeAudioStreamPlayback::start(double p_from_pos) {
	if (active) {
		return;
	}

	active = true;

	begin_resample();
}

void HaxeAudioStreamPlayback::stop() {
	active = false;
}

bool HaxeAudioStreamPlayback::is_playing() const {
	return active;
}

int HaxeAudioStreamPlayback::get_loop_count() const {
	return 0;
}

double HaxeAudioStreamPlayback::get_playback_position() const {
	if (stream.is_null() || stream->mix_rate <= 0.0f) {
		return 0.0;
	}
	return (double)stream->read_frame.get() / (double)stream->mix_rate;
}

void HaxeAudioStreamPlayback::seek(double p_time) {
	// The queue belongs to the decoder's position, not to this playback; the
	// producer resets it through `reset_queue` when the decoder seeks.
}
