#pragma once

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

struct pico_decoded_frame
{
	AHardwareBuffer * hardware_buffer = nullptr;
	int width = 0;
	int height = 0;
	uint64_t frame_index = 0;
	bool valid = false;

	XrTime display_time = 0;

	int64_t t_pushed_to_decoder_ns = 0;
	int64_t t_dequeued_output_ns = 0;
	int64_t t_image_available_ns = 0;

	XrPosef server_pose[2]{};
	XrFovf server_fov[2]{};
	std::array<wivrn::to_headset::foveation_parameter, 2> foveation;

	~pico_decoded_frame()
	{
		if (hardware_buffer)
			AHardwareBuffer_release(hardware_buffer);
	}
};

class pico_video_decoder
{
public:
	using frame_callback = std::function<void(std::shared_ptr<pico_decoded_frame>)>;

private:
	uint8_t stream_index;
	wivrn::video_codec codec_type;

	std::shared_ptr<AImageReader> image_reader;

	AMediaCodec * media_codec = nullptr;

	std::vector<uint8_t> csd_data;
	std::atomic<bool> csd_sent{false};

	struct pending_frame
	{
		std::vector<uint8_t> data;
		uint64_t frame_index = 0;
		int64_t t_received_ns = 0;
	};
	std::mutex pending_mutex;
	std::vector<pending_frame> pending_frames;
	std::condition_variable pending_cv;

	struct frame_info
	{
		uint64_t frame_index;
		wivrn::from_headset::feedback feedback;
		wivrn::to_headset::video_stream_data_shard::view_info_t view_info;
		int64_t t_pushed_to_decoder_ns = 0;
		int64_t t_dequeued_output_ns = 0;
	};
	std::mutex frame_info_mutex;
	std::vector<frame_info> pending_frame_infos;

	std::atomic<bool> exiting = false;
	std::atomic<bool> flushing = false;
	std::thread input_worker;
	std::thread output_worker;
	bool use_async = false;

	frame_callback on_frame_decoded;

	void on_image_available(AImageReader * reader);
	static void on_image_available_cb(void * ctx, AImageReader * reader);

	void input_loop();
	void output_loop();

	static void on_async_input_available(AMediaCodec * codec, void * userdata, int32_t index);
	static void on_async_output_available(AMediaCodec * codec, void * userdata, int32_t index, AMediaCodecBufferInfo * info);
	static void on_async_format_changed(AMediaCodec * codec, void * userdata, AMediaFormat * format);
	static void on_async_error(AMediaCodec * codec, void * userdata, media_status_t err, int32_t action_code, const char * detail);

public:
	pico_video_decoder(
		const wivrn::to_headset::video_stream_description & desc,
		uint8_t stream_idx,
		frame_callback callback);

	~pico_video_decoder();

	void push_data(std::span<std::span<const uint8_t>> data, uint64_t frame_index, bool partial);

	void flush();

	void frame_completed(
		const wivrn::from_headset::feedback & feedback,
		const wivrn::to_headset::video_stream_data_shard::view_info_t & view_info);

	static void supported_codecs(std::vector<wivrn::video_codec> & result);
};
