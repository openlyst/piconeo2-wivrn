#pragma once

/*
 * Video decoder for Pico Neo 2 WiVRn client.
 * Uses Android MediaCodec with AImageReader for hardware-accelerated
 * H.264/H.265 decoding to AHardwareBuffer for EGL display.
 */

#include "wivrn_packets.h"

#include <android/hardware_buffer.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>

struct neo2_decoded_frame
{
	AHardwareBuffer * hw_buffer = nullptr;
	int width = 0;
	int height = 0;
	uint64_t frame_index = 0;
	bool valid = false;

	~neo2_decoded_frame()
	{
		if (hw_buffer)
			AHardwareBuffer_release(hw_buffer);
	}
};

class neo2_video_decoder
{
public:
	using frame_callback = std::function<void(std::shared_ptr<neo2_decoded_frame>)>;

private:
	uint8_t stream_idx;

	std::shared_ptr<AImageReader> image_reader;
	AMediaCodec * codec = nullptr;

	struct queued_frame
	{
		std::vector<uint8_t> data;
		uint64_t frame_index = 0;
	};
	std::mutex queue_mutex;
	std::vector<queued_frame> queued_frames;
	std::condition_variable queue_cv;

	struct frame_metadata
	{
		uint64_t frame_index;
		wivrn::from_headset::feedback feedback;
		wivrn::to_headset::video_stream_data_shard::view_info_t view_info;
	};
	std::mutex metadata_mutex;
	std::vector<frame_metadata> pending_metadata;

	std::atomic<bool> stopping = false;
	std::thread decode_thread;

	frame_callback on_frame_ready;

	void handle_image_available(AImageReader * reader);
	static void image_available_cb(void * ctx, AImageReader * reader);

	void decode_loop();

public:
	neo2_video_decoder(
		const wivrn::to_headset::video_stream_description & desc,
		uint8_t stream_index,
		frame_callback callback);

	~neo2_video_decoder();

	void submit_data(std::span<std::span<const uint8_t>> data, uint64_t frame_index, bool partial);

	void mark_frame_complete(
		const wivrn::from_headset::feedback & feedback,
		const wivrn::to_headset::video_stream_data_shard::view_info_t & view_info);

	static void enumerate_supported_codecs(std::vector<wivrn::video_codec> & result);
};
