#pragma once

#include <spdlog/spdlog.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <vector>

#include <openxr/openxr.h>

struct stutter_event
{
	int64_t timestamp_ns;
	const char * stage;
	const char * reason;
	int64_t value_ns;
	uint64_t frame_index;
};

class stutter_detector
{
public:
	static constexpr int64_t TARGET_FRAME_NS = 13'888'888; // 72fps
	static constexpr int64_t STUTTER_THRESHOLD_NS = 20'000'000; // 20ms = missed deadline
	static constexpr int64_t STALE_FRAME_THRESHOLD_NS = 40'000'000; // 40ms old = stale
	static constexpr int64_t DECODE_SLOW_THRESHOLD_NS = 15'000'000; // 15ms decode = slow
	static constexpr int64_t ASSEMBLY_SLOW_THRESHOLD_NS = 10'000'000; // 10ms shard assembly = slow
	static constexpr float POSE_JUMP_THRESHOLD = 0.05f; // 5cm position jump or ~3deg rotation = visible snap
	static constexpr size_t HISTORY_SIZE = 120;

private:
	struct frame_record
	{
		uint64_t frame_index = 0;
		int64_t first_shard_ns = 0;
		int64_t last_shard_ns = 0;
		int64_t pushed_to_decoder_ns = 0;
		int64_t decoded_ns = 0;
		int64_t picked_for_render_ns = 0;
		int stream = -1;
	};

	std::mutex records_mutex;
	std::deque<frame_record> records;

	std::mutex frame_begin_mutex;
	int64_t last_frame_begin_ns = 0;
	uint64_t last_rendered_frame_index[2] = {0, 0};
	int64_t running_avg = 0;

	XrPosef last_pose[2] = {};
	uint64_t last_pose_frame[2] = {0, 0};
	bool last_pose_valid[2] = {false, false};

	std::atomic<int> stutter_count{0};
	std::atomic<int> total_frames{0};
	std::atomic<int> log_suppress_count{0};

	int64_t now_ns()
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return (int64_t)ts.tv_sec * 1'000'000'000LL + ts.tv_nsec;
	}

	void log_stutter(const char * stage, const char * reason, int64_t value_ns, uint64_t frame_index)
	{
		stutter_count.fetch_add(1, std::memory_order_relaxed);
		if (log_suppress_count.fetch_add(1, std::memory_order_relaxed) % 300 != 0)
			return;
		spdlog::warn("[STUTTER] stage={} reason={} value={:.1f}ms frame={}",
			stage, reason, value_ns / 1'000'000.0f, frame_index);
	}

public:
	void on_shard_arrived(uint64_t frame_index, int stream, bool is_first, bool is_last)
	{
		int64_t t = now_ns();
		std::lock_guard lock(records_mutex);

		frame_record * rec = nullptr;
		for (auto & r : records)
		{
			if (r.frame_index == frame_index && r.stream == stream)
			{
				rec = &r;
				break;
			}
		}
		if (!rec)
		{
			records.push_back({frame_index, t, t, 0, 0, 0, stream});
			rec = &records.back();
		}
		else
		{
			rec->last_shard_ns = t;
		}

		if (is_last)
		{
			int64_t assembly_time = rec->last_shard_ns - rec->first_shard_ns;
			if (assembly_time > ASSEMBLY_SLOW_THRESHOLD_NS)
			{
				log_stutter("network", "shard assembly slow", assembly_time, frame_index);
			}
		}

		while (records.size() > 64)
			records.pop_front();
	}

	void on_pushed_to_decoder(uint64_t frame_index, int stream)
	{
		int64_t t = now_ns();
		std::lock_guard lock(records_mutex);
		for (auto & r : records)
		{
			if (r.frame_index == frame_index && r.stream == stream)
			{
				r.pushed_to_decoder_ns = t;
				if (r.first_shard_ns > 0)
				{
					int64_t wait = t - r.last_shard_ns;
					if (wait > 5'000'000)
						log_stutter("queue", "frame waited in queue before decoder", wait, frame_index);
				}
				return;
			}
		}
	}

	void on_frame_decoded(uint64_t frame_index, int stream)
	{
		int64_t t = now_ns();
		std::lock_guard lock(records_mutex);
		for (auto & r : records)
		{
			if (r.frame_index == frame_index && r.stream == stream)
			{
				r.decoded_ns = t;
				if (r.pushed_to_decoder_ns > 0)
				{
					int64_t decode_time = t - r.pushed_to_decoder_ns;
					if (decode_time > DECODE_SLOW_THRESHOLD_NS)
						log_stutter("decoder", "decode took too long", decode_time, frame_index);
				}
				return;
			}
		}
	}

	void on_frame_begin(uint64_t left_frame_index, uint64_t right_frame_index)
	{
		int64_t t = now_ns();
		total_frames.fetch_add(1, std::memory_order_relaxed);

		int64_t interval = 0;
		int64_t avg = TARGET_FRAME_NS;
		{
			std::lock_guard lock(frame_begin_mutex);
			if (last_frame_begin_ns > 0)
			{
				interval = t - last_frame_begin_ns;
				running_avg = running_avg == 0 ? interval : (running_avg * 7 + interval) / 8;
				avg = running_avg;
			}
			last_frame_begin_ns = t;
		}

		if (interval > STUTTER_THRESHOLD_NS && avg > 0 && avg < STUTTER_THRESHOLD_NS)
		{
			log_stutter("render", "frame begin interval exceeded threshold", interval, left_frame_index);
		}

		if (interval > 0 && avg > 0)
		{
			int64_t jitter = interval - avg;
			if (jitter > 6'000'000)
			{
				log_stutter("render", "frame interval jitter (vs avg)", jitter, left_frame_index);
			}
		}

		if (left_frame_index != right_frame_index && left_frame_index != 0 && right_frame_index != 0)
		{
			int64_t diff = (int64_t)left_frame_index - (int64_t)right_frame_index;
			if (diff < 0) diff = -diff;
			log_stutter("pose", "left/right frame index mismatch", diff * 10'000'000LL, left_frame_index);
		}

		for (int eye = 0; eye < 2; eye++)
		{
			uint64_t fi = (eye == 0) ? left_frame_index : right_frame_index;
			if (fi == last_rendered_frame_index[eye] && fi != 0)
			{
				log_stutter("display", "same decoded frame repeated (no new frame arrived)", interval, fi);
			}
			last_rendered_frame_index[eye] = fi;
		}
	}

	void on_pose_update(int eye, uint64_t frame_index, const XrPosef & pose)
	{
		std::lock_guard lock(frame_begin_mutex);
		if (!last_pose_valid[eye])
		{
			last_pose[eye] = pose;
			last_pose_frame[eye] = frame_index;
			last_pose_valid[eye] = true;
			return;
		}

		if (frame_index == last_pose_frame[eye])
			return;

		float dx = pose.position.x - last_pose[eye].position.x;
		float dy = pose.position.y - last_pose[eye].position.y;
		float dz = pose.position.z - last_pose[eye].position.z;
		float pos_jump = std::sqrt(dx*dx + dy*dy + dz*dz);

		float qdx = pose.orientation.x - last_pose[eye].orientation.x;
		float qdy = pose.orientation.y - last_pose[eye].orientation.y;
		float qdz = pose.orientation.z - last_pose[eye].orientation.z;
		float qdw = pose.orientation.w - last_pose[eye].orientation.w;
		float rot_delta = std::sqrt(qdx*qdx + qdy*qdy + qdz*qdz + qdw*qdw);

		uint64_t frame_gap = frame_index - last_pose_frame[eye];
		float pos_per_frame = pos_jump / std::max((uint64_t)1, frame_gap);
		float rot_per_frame = rot_delta / std::max((uint64_t)1, frame_gap);

		if (pos_per_frame > POSE_JUMP_THRESHOLD)
		{
			log_stutter("pose", "position jump between frames", (int64_t)(pos_per_frame * 1'000'000'000), frame_index);
		}
		if (rot_per_frame > POSE_JUMP_THRESHOLD)
		{
			log_stutter("pose", "rotation jump between frames", (int64_t)(rot_per_frame * 1'000'000'000), frame_index);
		}

		last_pose[eye] = pose;
		last_pose_frame[eye] = frame_index;
	}

	void on_frame_end()
	{
		// could track frame end vs begin for total render time
	}

	void log_summary()
	{
		int total = total_frames.load(std::memory_order_relaxed);
		int stutters = stutter_count.load(std::memory_order_relaxed);
		if (total > 0 && total % 300 == 0)
		{
			spdlog::warn("[STUTTER SUMMARY] frames={} stutters={} rate={:.1f}%",
				total, stutters, 100.0f * stutters / total);
		}
	}
};

extern stutter_detector g_stutter;
