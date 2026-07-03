#pragma once

/*
 * Audio handler for Pico Neo 2 WiVRn client.
 * Uses AAudio for low-latency speaker output and microphone input.
 */

#include "wivrn_packets.h"
#include "neo2_session.h"

#include <aaudio/AAudio.h>
#include <atomic>
#include <mutex>
#include <thread>

class neo2_audio
{
	static int32_t speaker_data_cb(AAudioStream * stream, void * userdata, void * audio_data, int32_t num_frames);
	static int32_t mic_data_cb(AAudioStream * stream, void * userdata, void * audio_data, int32_t num_frames);
	static void speaker_error_cb(AAudioStream * stream, void * userdata, aaudio_result_t error);
	static void mic_error_cb(AAudioStream * stream, void * userdata, aaudio_result_t error);

	void build_speaker_stream(AAudioStreamBuilder * builder, int32_t sample_rate, int32_t num_channels);
	void build_mic_stream(AAudioStreamBuilder * builder, int32_t sample_rate, int32_t num_channels);

	void recreate_stream(AAudioStream * stream);

	AAudioStream * speaker = nullptr;
	AAudioStream * microphone = nullptr;

	std::atomic<bool> speaker_stop_ack{false};
	std::atomic<bool> mic_stop_ack{false};
	std::atomic<bool> mic_active{false};
	std::atomic<bool> stopping{false};

	neo2_session & session;

	std::mutex mutex;
	std::thread recreate_thread;

	wivrn::audio_data speaker_remainder;
	std::atomic<size_t> buffer_bytes{0};

	static constexpr size_t ring_capacity = 100;
	wivrn::audio_data ring_buffer[ring_capacity];
	std::atomic<size_t> ring_read{0};
	std::atomic<size_t> ring_write{0};

	void shutdown();

	int64_t timestamp_ns()
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
	}

public:
	neo2_audio(const wivrn::to_headset::audio_stream_description & desc, neo2_session & sess);
	~neo2_audio();

	void operator()(wivrn::audio_data && data);
	void set_microphone_active(bool running);

	static void fill_audio_info(wivrn::from_headset::headset_info_packet & info);
};
