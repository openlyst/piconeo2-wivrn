#include "pico_decoder.h"

#include <spdlog/spdlog.h>
#include <cstring>
#include <cstdio>
#include <chrono>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <media/NdkMediaError.h>

namespace
{
const char * mime(wivrn::video_codec codec)
{
	switch (codec)
	{
		case wivrn::video_codec::h264:
			return "video/avc";
		case wivrn::video_codec::h265:
			return "video/hevc";
		case wivrn::video_codec::av1:
			return "video/av01";
		case wivrn::video_codec::raw:
			break;
	}
	__builtin_unreachable();
}

struct nal_unit
{
	const uint8_t * data;
	size_t size;
	int type;
};

static int find_nal_units(const uint8_t * buf, size_t buf_size,
                          std::vector<nal_unit> & out)
{
	size_t i = 0;
	while (i + 3 < buf_size)
	{
		size_t sc_len = 0;
		if (i + 3 < buf_size && buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1)
			sc_len = 3;
		else if (i + 4 < buf_size && buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 0 && buf[i+3] == 1)
			sc_len = 4;

		if (sc_len == 0)
		{
			i++;
			continue;
		}

		size_t nal_start = i + sc_len;
		size_t j = nal_start + 1;
		while (j + 3 < buf_size)
		{
			if (buf[j] == 0 && buf[j+1] == 0 && (buf[j+2] == 1 || (j + 4 < buf_size && buf[j+2] == 0 && buf[j+3] == 1)))
				break;
			j++;
		}
		size_t nal_end = (j + 3 < buf_size) ? j : buf_size;

		int nal_type = -1;
		if (nal_start < buf_size)
		{
			nal_type = buf[nal_start] & 0x1f;
		}

		out.push_back({buf + nal_start, nal_end - nal_start, nal_type});
		i = nal_end;
	}
	return (int)out.size();
}

static bool is_csd_nal_h264(int nal_type)
{
	return nal_type == 7 || nal_type == 8;
}

static bool is_csd_nal_h265(int nal_type)
{
	return nal_type == 32 || nal_type == 33 || nal_type == 34;
}

static void extract_csd(const uint8_t * data, size_t size, wivrn::video_codec codec,
                        std::vector<uint8_t> & csd_out)
{
	std::vector<nal_unit> nals;
	find_nal_units(data, size, nals);

	static int csd_call_count = 0;
	if (++csd_call_count % 300 == 1)
		spdlog::warn("extract_csd: {} NALs found in {} bytes, codec={}",
			nals.size(), size, (int)codec);

	for (auto & n : nals)
	{
		bool is_csd = false;
		if (codec == wivrn::video_codec::h264)
			is_csd = is_csd_nal_h264(n.type);
		else if (codec == wivrn::video_codec::h265)
		{
			int h265_type = (n.data[0] >> 1) & 0x3f;
			is_csd = is_csd_nal_h265(h265_type);
		}

		if (is_csd)
		{
			const uint8_t * nal_start = n.data;
			while (nal_start > data && *(nal_start - 1) == 0)
				nal_start--;

			csd_out.insert(csd_out.end(), nal_start, n.data + n.size);
		}
	}
}

static std::vector<uint8_t> strip_csd_from_frame(const uint8_t * data, size_t size,
                                                  wivrn::video_codec codec)
{
	std::vector<nal_unit> nals;
	find_nal_units(data, size, nals);

	std::vector<uint8_t> result;
	for (auto & n : nals)
	{
		bool is_csd = false;
		if (codec == wivrn::video_codec::h264)
			is_csd = is_csd_nal_h264(n.type);
		else if (codec == wivrn::video_codec::h265)
		{
			int h265_type = (n.data[0] >> 1) & 0x3f;
			is_csd = is_csd_nal_h265(h265_type);
		}

		if (!is_csd)
		{
			const uint8_t * nal_start = n.data;
			size_t back = 0;
			while (back < 4 && nal_start - 1 - back >= data && *(nal_start - 1 - back) == 0)
				back++;
			if (back >= 3)
				result.insert(result.end(), nal_start - back, nal_start + n.size);
			else
				result.insert(result.end(), n.data, n.data + n.size);
		}
	}
	return result;
}

void check(media_status_t status, const char * msg)
{
	if (status != AMEDIA_OK)
	{
		spdlog::error("{}: MediaCodec error {}", msg, (int)status);
		throw std::runtime_error("MediaCodec error");
	}
}

PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHRProc = nullptr;
PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHRProc = nullptr;
PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC eglGetNativeClientBufferANDROIDProc = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOESProc = nullptr;

void load_egl_procs()
{
	if (eglCreateImageKHRProc)
		return;

	eglCreateImageKHRProc = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	eglDestroyImageKHRProc = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	eglGetNativeClientBufferANDROIDProc = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
	glEGLImageTargetTexture2DOESProc = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

	if (!eglCreateImageKHRProc || !eglDestroyImageKHRProc)
		spdlog::warn("EGL_KHR_image not available, video may not work");
	if (!eglGetNativeClientBufferANDROIDProc)
		spdlog::warn("EGL_ANDROID_get_native_client_buffer not available, video may not work");
	if (!glEGLImageTargetTexture2DOESProc)
		spdlog::warn("GL_OES_EGL_image not available, video may not work");
}

void release_hardware_buffer(AHardwareBuffer * hb)
{
	if (hb)
		AHardwareBuffer_release(hb);
}

static int64_t now_ns()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
} // namespace

pico_video_decoder::pico_video_decoder(
	const wivrn::to_headset::video_stream_description & desc,
	uint8_t stream_idx,
	frame_callback callback) :
	stream_index(stream_idx),
	codec_type(desc.codec[stream_idx]),
	on_frame_decoded(std::move(callback))
{
	load_egl_procs();

	auto width = desc.width;
	auto height = desc.height / (stream_index == 2 ? 2 : 1);

	AImageReader * ir;
	check(AImageReader_newWithUsage(
		      width,
		      height,
		      AIMAGE_FORMAT_PRIVATE,
		      AHARDWAREBUFFER_USAGE_CPU_READ_NEVER | AHARDWAREBUFFER_USAGE_CPU_WRITE_NEVER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
		      8,
		      &ir),
	      "AImageReader_newWithUsage");
	image_reader.reset(ir, [](AImageReader * r) { AImageReader_delete(r); });

	AImageReader_ImageListener listener{this, on_image_available_cb};
	check(AImageReader_setImageListener(ir, &listener), "AImageReader_setImageListener");

	int fps = (int)std::ceil(desc.frame_rate);

	AMediaFormat * format = AMediaFormat_new();
	AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime(codec_type));
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
#if __ANDROID_API__ >= 28
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_OPERATING_RATE, fps);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_PRIORITY, 0);
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, fps);
#endif
	AMediaFormat_setInt32(format, "vendor.qti-ext-dec-low-latency.enable", 1);
	AMediaFormat_setInt32(format, "low-latency", 1);

	media_codec = AMediaCodec_createDecoderByType(mime(codec_type));
	if (!media_codec)
		throw std::runtime_error(std::string("Cannot create decoder for ") + mime(codec_type));

	spdlog::warn("Created MediaCodec decoder for stream {} codec={} fps={}", stream_index, mime(codec_type), fps);

	ANativeWindow * window;
	check(AImageReader_getWindow(image_reader.get(), &window), "AImageReader_getWindow");

	check(AMediaCodec_configure(media_codec, format, window, nullptr, 0), "AMediaCodec_configure");

	AMediaCodecOnAsyncNotifyCallback async_cb = {
		.onAsyncInputAvailable = on_async_input_available,
		.onAsyncOutputAvailable = on_async_output_available,
		.onAsyncFormatChanged = on_async_format_changed,
		.onAsyncError = on_async_error,
	};
	check(AMediaCodec_setAsyncNotifyCallback(media_codec, async_cb, this), "AMediaCodec_setAsyncNotifyCallback");

	check(AMediaCodec_start(media_codec), "AMediaCodec_start");

	AMediaFormat_delete(format);
}

pico_video_decoder::~pico_video_decoder()
{
	exiting = true;
	pending_cv.notify_all();

	if (media_codec)
	{
		AMediaCodec_stop(media_codec);
		AMediaCodec_delete(media_codec);
	}
}

void pico_video_decoder::on_async_input_available(AMediaCodec * codec, void * userdata, int32_t index)
{
	auto * self = static_cast<pico_video_decoder *>(userdata);
	if (self->exiting || self->flushing.load())
		return;

	std::unique_lock lock(self->pending_mutex);
	if (self->pending_frames.empty())
		return;

	pending_frame frame = std::move(self->pending_frames.front());
	self->pending_frames.erase(self->pending_frames.begin());
	lock.unlock();

	size_t buf_size;
	uint8_t * buf = AMediaCodec_getInputBuffer(codec, index, &buf_size);
	if (!buf)
	{
		AMediaCodec_queueInputBuffer(codec, index, 0, 0, 0, 0);
		return;
	}

	std::vector<uint8_t> frame_data = std::move(frame.data);
	size_t copy_size = std::min(frame_data.size(), buf_size);
	memcpy(buf, frame_data.data(), copy_size);

	uint64_t timestamp = frame.frame_index * 10'000;
	AMediaCodec_queueInputBuffer(codec, index, 0, copy_size, timestamp, 0);
}

void pico_video_decoder::on_async_output_available(AMediaCodec * codec, void * userdata, int32_t index, AMediaCodecBufferInfo * info)
{
	auto * self = static_cast<pico_video_decoder *>(userdata);
	if (self->exiting || self->flushing.load())
		return;

	int64_t dequeue_time = now_ns();
	uint64_t ts_frame = (info->presentationTimeUs + 5'000) / 10'000;

	{
		std::lock_guard lock(self->frame_info_mutex);
		for (auto & fi : self->pending_frame_infos)
		{
			if (fi.frame_index == ts_frame)
			{
				fi.t_dequeued_output_ns = dequeue_time;
				break;
			}
		}
	}

	AMediaCodec_releaseOutputBuffer(codec, index, true);
}

void pico_video_decoder::on_async_format_changed(AMediaCodec * codec, void * userdata, AMediaFormat * format)
{
	spdlog::warn("MediaCodec format changed: {}", AMediaFormat_toString(format));
}

void pico_video_decoder::on_async_error(AMediaCodec * codec, void * userdata, media_status_t err, int32_t action_code, const char * detail)
{
	spdlog::error("MediaCodec async error: err={} action={} detail={}", (int)err, action_code, detail ? detail : "(null)");
}

void pico_video_decoder::push_data(std::span<std::span<const uint8_t>> data, uint64_t frame_index, bool partial)
{
	std::lock_guard lock(pending_mutex);

	pending_frame * pf = nullptr;
	if (!pending_frames.empty() && pending_frames.back().frame_index == frame_index)
	{
		pf = &pending_frames.back();
	}
	else
	{
		while (pending_frames.size() >= 5)
		{
			spdlog::warn("Decoder stream {} dropping frame {} (queue full)", stream_index, pending_frames.front().frame_index);
			pending_frames.erase(pending_frames.begin());
		}
		pending_frames.push_back({.frame_index = frame_index});
		pf = &pending_frames.back();
	}

	for (const auto & sub : data)
	{
		size_t old_size = pf->data.size();
		pf->data.resize(old_size + sub.size());
		memcpy(pf->data.data() + old_size, sub.data(), sub.size());
	}

	if (!partial)
	{
		pending_cv.notify_one();
	}
}

void pico_video_decoder::flush()
{
	flushing.store(true);
	spdlog::warn("Decoder stream {} flush requested", stream_index);

	AMediaCodec_flush(media_codec);

	{
		std::lock_guard lock(pending_mutex);
		pending_frames.clear();
	}
	{
		std::lock_guard lock(frame_info_mutex);
		pending_frame_infos.clear();
	}
	csd_sent.store(false);

	flushing.store(false);
	spdlog::warn("Decoder stream {} flush complete", stream_index);
}

void pico_video_decoder::frame_completed(
	const wivrn::from_headset::feedback & feedback,
	const wivrn::to_headset::video_stream_data_shard::view_info_t & view_info)
{
	std::lock_guard lock(frame_info_mutex);
	pending_frame_infos.push_back(frame_info{
		.frame_index = feedback.frame_index,
		.feedback = feedback,
		.view_info = view_info,
		.t_pushed_to_decoder_ns = now_ns(),
	});
}

void pico_video_decoder::on_image_available_cb(void * ctx, AImageReader * reader)
{
	auto * self = static_cast<pico_video_decoder *>(ctx);
	try
	{
		self->on_image_available(reader);
	}
	catch (std::exception & e)
	{
		spdlog::error("on_image_available: {}", e.what());
	}
}

void pico_video_decoder::on_image_available(AImageReader * reader)
{
	int64_t image_cb_time = now_ns();
	pid_t cb_tid = gettid();

	static int cb_count[2] = {0, 0};
	if (++cb_count[stream_index] % 300 == 1 || cb_count[stream_index] <= 3)
		spdlog::warn("SCHED_DIAG s={} image_cb tid={} count={}", stream_index, (int)cb_tid, cb_count[stream_index]);

	AImage * tmp = nullptr;
	media_status_t acq_status = AImageReader_acquireLatestImage(image_reader.get(), &tmp);
	if (acq_status == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE)
		return;
	check(acq_status, "AImageReader_acquireLatestImage");
	std::shared_ptr<AImage> image(tmp, [](AImage * img) { AImage_delete(img); });

	int64_t fake_timestamp_ns;
	check(AImage_getTimestamp(image.get(), &fake_timestamp_ns), "AImage_getTimestamp");
	uint64_t frame_index = (fake_timestamp_ns + 5'000'000) / 10'000'000;

	frame_info info{};
	bool found = false;
	{
		std::lock_guard lock(frame_info_mutex);
		for (auto it = pending_frame_infos.begin(); it != pending_frame_infos.end(); ++it)
		{
			if (it->frame_index == frame_index)
			{
				info = *it;
				found = true;
				pending_frame_infos.erase(it);
				break;
			}
		}
		while (!pending_frame_infos.empty() && pending_frame_infos.front().frame_index < frame_index)
			pending_frame_infos.erase(pending_frame_infos.begin());
	}

	if (!found)
	{
		spdlog::warn("No frame info for decoded frame {}, dropping", frame_index);
		return;
	}

	AHardwareBuffer * hb = nullptr;
	check(AImage_getHardwareBuffer(image.get(), &hb), "AImage_getHardwareBuffer");
	if (!hb)
	{
		spdlog::warn("No hardware buffer in decoded image");
		return;
	}

	AHardwareBuffer_Desc desc{};
	AHardwareBuffer_describe(hb, &desc);

	AHardwareBuffer_acquire(hb);

	auto frame = std::make_shared<pico_decoded_frame>();
	frame->hardware_buffer = hb;
	frame->width = desc.width;
	frame->height = desc.height;
	frame->frame_index = frame_index;
	frame->valid = true;
	frame->display_time = info.view_info.display_time;
	frame->t_pushed_to_decoder_ns = info.t_pushed_to_decoder_ns;
	frame->t_dequeued_output_ns = info.t_dequeued_output_ns;
	frame->t_image_available_ns = image_cb_time;

	for (int i = 0; i < 2; i++)
	{
		frame->server_pose[i] = info.view_info.pose[i];
		frame->server_fov[i] = info.view_info.fov[i];
		frame->foveation[i] = info.view_info.foveation[i];
	}

	static int decoded_count[2] = {0, 0};
	if (++decoded_count[stream_index] % 300 == 1 || decoded_count[stream_index] <= 5)
	{
		int64_t total_latency = image_cb_time - info.t_pushed_to_decoder_ns;
		int64_t decode_latency = info.t_dequeued_output_ns - info.t_pushed_to_decoder_ns;
		int64_t image_latency = image_cb_time - info.t_dequeued_output_ns;
		spdlog::warn("DEC_LATENCY s={} frame={} | TOTAL={:.1f}ms decode={:.1f}ms release_to_image={:.1f}ms ({}x{})",
			stream_index, frame_index,
			total_latency / 1e6f, decode_latency / 1e6f, image_latency / 1e6f,
			desc.width, desc.height);
	}

	if (on_frame_decoded)
		on_frame_decoded(frame);
}

void pico_video_decoder::supported_codecs(std::vector<wivrn::video_codec> & result)
{
	for (auto codec : {wivrn::video_codec::av1, wivrn::video_codec::h265, wivrn::video_codec::h264})
	{
		AMediaCodec * mc = AMediaCodec_createDecoderByType(mime(codec));
		bool supported = mc != nullptr;
		if (mc)
			AMediaCodec_delete(mc);
		if (supported)
			result.push_back(codec);
		spdlog::info("Video codec {}: {}supported", (int)codec, supported ? "" : "NOT ");
	}
}
