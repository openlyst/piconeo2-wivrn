/*
 * Video decoder implementation for Pico Neo 2 WiVRn client.
 * Uses Android MediaCodec for hardware decoding and AImageReader
 * to get AHardwareBuffer from decoded frames for EGL texturing.
 */

#include "neo2_video_decoder.h"

#include <spdlog/spdlog.h>
#include <cstring>
#include <chrono>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

namespace
{
const char * codec_mime(wivrn::video_codec c)
{
	switch (c)
	{
		case wivrn::video_codec::h264: return "video/avc";
		case wivrn::video_codec::h265: return "video/hevc";
		case wivrn::video_codec::av1:  return "video/av01";
		case wivrn::video_codec::raw:  break;
	}
	__builtin_unreachable();
}

void check_media_status(media_status_t status, const char * msg)
{
	if (status != AMEDIA_OK)
	{
		spdlog::error("{}: MediaCodec error {}", msg, (int)status);
		throw std::runtime_error("MediaCodec error");
	}
}

PFNEGLCREATEIMAGEKHRPROC egl_create_image = nullptr;
PFNEGLDESTROYIMAGEKHRPROC egl_destroy_image = nullptr;
PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC egl_get_native_buffer = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gl_egl_image_target = nullptr;

void load_egl_extensions()
{
	if (egl_create_image)
		return;

	egl_create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	egl_destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	egl_get_native_buffer = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
	gl_egl_image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

	if (!egl_create_image || !egl_destroy_image)
		spdlog::warn("EGL_KHR_image not available");
	if (!egl_get_native_buffer)
		spdlog::warn("EGL_ANDROID_get_native_client_buffer not available");
	if (!gl_egl_image_target)
		spdlog::warn("GL_OES_EGL_image not available");
}
} // namespace

neo2_video_decoder::neo2_video_decoder(
	const wivrn::to_headset::video_stream_description & desc,
	uint8_t stream_index,
	frame_callback callback) :
	stream_idx(stream_index),
	on_frame_ready(std::move(callback))
{
	load_egl_extensions();

	auto width = desc.width;
	auto height = desc.height / (stream_index == 2 ? 2 : 1);

	AImageReader * ir;
	check_media_status(AImageReader_newWithUsage(
		width, height, AIMAGE_FORMAT_PRIVATE,
		AHARDWAREBUFFER_USAGE_CPU_READ_NEVER | AHARDWAREBUFFER_USAGE_CPU_WRITE_NEVER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
		8, &ir),
		"AImageReader_newWithUsage");
	image_reader.reset(ir, [](AImageReader * r) { AImageReader_delete(r); });

	AImageReader_ImageListener listener{this, image_available_cb};
	check_media_status(AImageReader_setImageListener(ir, &listener), "AImageReader_setImageListener");

	AMediaFormat * format = AMediaFormat_new();
	AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, codec_mime(desc.codec[stream_index]));
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
#if __ANDROID_API__ >= 28
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_OPERATING_RATE, (int32_t)std::ceil(desc.frame_rate));
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_PRIORITY, 0);
#endif

	codec = AMediaCodec_createDecoderByType(codec_mime(desc.codec[stream_index]));
	if (!codec)
		throw std::runtime_error(std::string("Cannot create decoder for ") + codec_mime(desc.codec[stream_index]));

	spdlog::info("Created MediaCodec decoder for stream {}", stream_index);

	ANativeWindow * window;
	check_media_status(AImageReader_getWindow(image_reader.get(), &window), "AImageReader_getWindow");

	check_media_status(AMediaCodec_configure(codec, format, window, nullptr, 0), "AMediaCodec_configure");
	check_media_status(AMediaCodec_start(codec), "AMediaCodec_start");

	AMediaFormat_delete(format);

	decode_thread = std::thread([this]() { decode_loop(); });
}

neo2_video_decoder::~neo2_video_decoder()
{
	stopping = true;
	queue_cv.notify_all();

	if (codec)
	{
		AMediaCodec_stop(codec);
		AMediaCodec_delete(codec);
	}

	if (decode_thread.joinable())
		decode_thread.join();
}

void neo2_video_decoder::decode_loop()
{
	while (!stopping)
	{
		ssize_t in_idx = AMediaCodec_dequeueInputBuffer(codec, 1000);
		if (in_idx >= 0)
		{
			size_t buf_size;
			uint8_t * buf = AMediaCodec_getInputBuffer(codec, in_idx, &buf_size);
			if (!buf)
			{
				AMediaCodec_queueInputBuffer(codec, in_idx, 0, 0, 0, 0);
				continue;
			}

			queued_frame frame;
			bool has_frame = false;
			{
				std::unique_lock lock(queue_mutex);
				if (queue_cv.wait_for(lock, std::chrono::milliseconds(10),
				    [&]() { return !queued_frames.empty() || stopping; }))
				{
					if (!queued_frames.empty())
					{
						frame = std::move(queued_frames.front());
						queued_frames.erase(queued_frames.begin());
						has_frame = true;
					}
				}
			}

			if (has_frame)
			{
				size_t copy_size = std::min(frame.data.size(), buf_size);
				memcpy(buf, frame.data.data(), copy_size);
				uint64_t timestamp = frame.frame_index * 10'000;
				auto status = AMediaCodec_queueInputBuffer(codec, in_idx, 0, copy_size, timestamp, 0);
				if (status != AMEDIA_OK)
					spdlog::error("queueInputBuffer: error {}", (int)status);
			}
			else
			{
				AMediaCodec_queueInputBuffer(codec, in_idx, 0, 0, 0, 0);
			}
		}

		AMediaCodecBufferInfo info;
		ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(codec, &info, 1000);
		if (out_idx >= 0)
		{
			auto status = AMediaCodec_releaseOutputBuffer(codec, out_idx, true);
			if (status != AMEDIA_OK)
				spdlog::error("releaseOutputBuffer: error {}", (int)status);
		}
		else if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
		{
			AMediaFormat * fmt = AMediaCodec_getOutputFormat(codec);
			spdlog::info("MediaCodec output format: {}", AMediaFormat_toString(fmt));
			AMediaFormat_delete(fmt);
		}
	}
}

void neo2_video_decoder::submit_data(std::span<std::span<const uint8_t>> data, uint64_t frame_index, bool partial)
{
	std::lock_guard lock(queue_mutex);

	queued_frame * pf = nullptr;
	if (!queued_frames.empty() && queued_frames.back().frame_index == frame_index)
	{
		pf = &queued_frames.back();
	}
	else
	{
		queued_frames.push_back({.frame_index = frame_index});
		pf = &queued_frames.back();
	}

	for (const auto & sub : data)
	{
		size_t old_size = pf->data.size();
		pf->data.resize(old_size + sub.size());
		memcpy(pf->data.data() + old_size, sub.data(), sub.size());
	}

	if (!partial)
		queue_cv.notify_one();
}

void neo2_video_decoder::mark_frame_complete(
	const wivrn::from_headset::feedback & feedback,
	const wivrn::to_headset::video_stream_data_shard::view_info_t & view_info)
{
	std::lock_guard lock(metadata_mutex);
	pending_metadata.push_back(frame_metadata{
		.frame_index = feedback.frame_index,
		.feedback = feedback,
		.view_info = view_info,
	});
}

void neo2_video_decoder::image_available_cb(void * ctx, AImageReader * reader)
{
	auto * self = static_cast<neo2_video_decoder *>(ctx);
	try
	{
		self->handle_image_available(reader);
	}
	catch (std::exception & e)
	{
		spdlog::error("image_available: {}", e.what());
	}
}

void neo2_video_decoder::handle_image_available(AImageReader * reader)
{
	AImage * tmp;
	check_media_status(AImageReader_acquireLatestImage(image_reader.get(), &tmp), "AImageReader_acquireLatestImage");
	std::shared_ptr<AImage> image(tmp, [](AImage * img) { AImage_delete(img); });

	int64_t fake_ts;
	check_media_status(AImage_getTimestamp(image.get(), &fake_ts), "AImage_getTimestamp");
	uint64_t frame_index = (fake_ts + 5'000'000) / 10'000'000;

	frame_metadata info{};
	bool found = false;
	{
		std::lock_guard lock(metadata_mutex);
		for (auto it = pending_metadata.begin(); it != pending_metadata.end(); ++it)
		{
			if (it->frame_index == frame_index)
			{
				info = *it;
				found = true;
				pending_metadata.erase(it);
				break;
			}
		}
		while (!pending_metadata.empty() && pending_metadata.front().frame_index < frame_index)
			pending_metadata.erase(pending_metadata.begin());
	}

	if (!found)
	{
		spdlog::warn("No metadata for decoded frame {}, dropping", frame_index);
		return;
	}

	AHardwareBuffer * hb = nullptr;
	check_media_status(AImage_getHardwareBuffer(image.get(), &hb), "AImage_getHardwareBuffer");
	if (!hb)
	{
		spdlog::warn("No hardware buffer in decoded image");
		return;
	}

	AHardwareBuffer_Desc desc{};
	AHardwareBuffer_describe(hb, &desc);
	AHardwareBuffer_acquire(hb);

	auto frame = std::make_shared<neo2_decoded_frame>();
	frame->hw_buffer = hb;
	frame->width = desc.width;
	frame->height = desc.height;
	frame->frame_index = frame_index;
	frame->valid = true;

	spdlog::debug("Decoded frame {} available ({}x{})", frame_index, desc.width, desc.height);

	if (on_frame_ready)
		on_frame_ready(frame);
}

void neo2_video_decoder::enumerate_supported_codecs(std::vector<wivrn::video_codec> & result)
{
	for (auto codec : {wivrn::video_codec::av1, wivrn::video_codec::h265, wivrn::video_codec::h264})
	{
		AMediaCodec * mc = AMediaCodec_createDecoderByType(codec_mime(codec));
		bool supported = mc != nullptr;
		if (mc)
			AMediaCodec_delete(mc);
		if (supported)
			result.push_back(codec);
		spdlog::info("Video codec {}: {}supported", (int)codec, supported ? "" : "NOT ");
	}
}
