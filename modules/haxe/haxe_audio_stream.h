/**************************************************************************/
/*  haxe_audio_stream.h                                                   */
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

#pragma once

#include "core/object/object.h"
#include "core/templates/safe_refcount.h"
#include "core/templates/vector.h"
#include "servers/audio/audio_stream.h"

/*
 * A pull based audio stream fed by a media decoder from another thread.
 *
 * `AudioStreamGenerator` is the engine's user fed stream, but it is meant to be
 * filled from the main loop and its ring buffer has no synchronization, so
 * pushing decoded audio into it from a decoder thread would race the audio
 * thread. It also has to be kept almost full, which for a video player means
 * the sound trails the picture by the whole buffer, and it only advances when
 * the main loop runs.
 *
 * This stream is the opposite. A producer (a media decoder's audio callback)
 * writes into a single producer / single consumer ring and the audio thread
 * drains it in real time, independently of the frame rate. The picture and the
 * sound are then paced by the same real time clock instead of by the render
 * loop, which is what keeps them together.
 *
 * The consumer plays the *oldest* frame in the ring, so what is queued is audio
 * the decoder has run ahead on, not a delay in front of the sound being heard.
 * That matters because a decoder hands over a burst of well over a second when
 * a media is opened (libvlc decodes ahead to absorb jitter), and all of it
 * still has to be played, in order, at the position it belongs to. The ring
 * therefore has to be large enough to hold the read-ahead; one that is not
 * forces the surplus either out of the ring or out of the front of the ring,
 * and either way the sound is no longer where the picture is.
 *
 * All cursors are frame counts since the stream was created, not positions in
 * the ring, so the two threads never have to agree on a shared index. The
 * consumer is the only writer of the read cursor and the producer the only
 * writer of the write cursor; both publish with `SafeNumeric` (release) and
 * read (acquire) so a consumer can never observe a frame that is still being
 * written.
 */
class HaxeAudioStream : public AudioStream {
	GDSOFTCLASS(HaxeAudioStream, AudioStream);
	// The playback reports the stream's rate and its playback position, so it
	// reads the queue's cursors directly rather than through copies.
	friend class HaxeAudioStreamPlayback;

	/* Sample rate of the samples handed to `write_frames`. */
	float mix_rate = 44100.0f;

	/* Seconds of audio the ring can hold. It has to fit the decoder's
	 * read-ahead, not the steady state: see the class comment. */
	float buffer_len = 1.5f;

	/* Frames the consumer keeps queued before it starts playing. This one is a
	 * margin against a producer that is momentarily behind, so it is small. */
	int target_frames = 0;

	/* A backlog this large means the read-ahead does not fit the ring, so
	 * something has to give. The consumer skips the oldest frames down to
	 * `target_frames`, which pulls the sound forward to where the picture is
	 * instead of letting it trail behind for good. A decoder paced in real time
	 * never gets near this. */
	int high_water_frames = 0;

	Vector<AudioFrame> ring;
	uint32_t ring_mask = 0;

	SafeNumeric<uint64_t> write_frame; // Frames produced, ever.
	SafeNumeric<uint64_t> read_frame; // Frames consumed, ever.
	SafeNumeric<uint64_t> dropped_frames; // Frames refused because the ring was full.
	SafeNumeric<uint64_t> underrun_count; // Times a mix found the ring short.
	SafeNumeric<uint64_t> trimmed_frames; // Frames skipped to bound a runaway backlog.
	/* Non zero while the consumer holds silence to build up its initial
	 * backlog (armed when the stream is created) and again after a flush (a
	 * seek, or the start of a loop). */
	SafeNumeric<uint32_t> warming;

	void _resize_ring();

public:
	HaxeAudioStream();
	~HaxeAudioStream();

	/* Sets the sample rate, the ring capacity and the backlog the consumer
	 * aims for, then sizes the ring. Called once when the stream is created,
	 * before anything can write to it. */
	void configure(double p_mix_rate, double p_buffer_len, double p_target_len);

	double get_mix_rate() const { return mix_rate; }

	/*
	 * Writes interleaved signed 16 bit frames. Called from the producer thread
	 * only. Returns the number of frames taken; a short write means the ring
	 * was full and the surplus was dropped. Never blocks.
	 */
	int write_frames(const int16_t *p_samples, int p_frames, int p_channels);

	/* Frames queued right now: the decoder's read-ahead, i.e. how far ahead of
	 * the sound being heard the samples already handed over reach. */
	int64_t buffered_frames() const;

	/*
	 * Pulls the next `p_frames` frames into `p_buffer`. Called on the audio
	 * thread through the playback; kept here because the queue and the rules
	 * that drain it belong together. It always fills as many frames as asked
	 * for (silence when the queue runs short): a shorter count would make the
	 * audio server delete the playback like a finished stream.
	 */
	int pull_frames(AudioFrame *p_buffer, int p_frames);

	/* Discards everything queued and asks the consumer to build its backlog
	 * again. Used on a seek or the start of a loop. */
	void reset_queue();

	uint64_t get_dropped_frames() const { return dropped_frames.get(); }
	uint64_t get_underrun_count() const { return underrun_count.get(); }
	uint64_t get_trimmed_frames() const { return trimmed_frames.get(); }

	virtual Ref<AudioStreamPlayback> instantiate_playback() override;
	virtual String get_stream_name() const override;

	virtual double get_length() const override;
	virtual bool is_monophonic() const override;
};

/*
 * The playback pulls from its stream's queue on the audio thread. It keeps no
 * queue of its own, so restarting the player (a loop, or a seek) does not throw
 * away audio the decoder has already produced.
 */
class HaxeAudioStreamPlayback : public AudioStreamPlaybackResampled {
	GDSOFTCLASS(HaxeAudioStreamPlayback, AudioStreamPlaybackResampled);
	friend class HaxeAudioStream;

	Ref<HaxeAudioStream> stream;
	bool active = false;

protected:
	virtual int _mix_internal(AudioFrame *p_buffer, int p_frames) override;
	virtual float get_stream_sampling_rate() override;

public:
	HaxeAudioStreamPlayback();

	virtual void start(double p_from_pos = 0.0) override;
	virtual void stop() override;
	virtual bool is_playing() const override;

	virtual int get_loop_count() const override;
	virtual double get_playback_position() const override;
	virtual void seek(double p_time) override;
};
