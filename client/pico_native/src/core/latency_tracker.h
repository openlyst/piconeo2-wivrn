#pragma once

#include <spdlog/spdlog.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <vector>

class latency_tracker
{
public:
	static constexpr size_t HISTORY_SIZE = 180;
	static constexpr int SUMMARY_INTERVAL = 90;

private:
	struct frame_timing
	{
		uint64_t frame_index = 0;
		int stream = -1;

		int64_t encode_begin = 0;
		int64_t encode_end = 0;
		int64_t send_begin = 0;
		int64_t send_end = 0;

		int64_t recv_first = 0;
		int64_t recv_last = 0;
		int64_t decoder_in = 0;
		int64_t decoded = 0;
		int64_t rendered = 0;
		int64_t submitted = 0;
		int64_t predicted_display = 0;

		bool has_server_timing = false;
		bool complete = false;
	};

	std::mutex mutex;
	std::deque<frame_timing> records;

	std::atomic<int> completed_count{0};
	std::atomic<int> logged_count{0};

	int64_t now_ns()
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return (int64_t)ts.tv_sec * 1'000'000'000LL + ts.tv_nsec;
	}

	frame_timing * find(uint64_t frame_index, int stream)
	{
		for (auto & r : records)
			if (r.frame_index == frame_index && r.stream == stream)
				return &r;
		return nullptr;
	}

	void trim()
	{
		while (records.size() > HISTORY_SIZE)
			records.pop_front();
	}

	struct stage_stats
	{
		float min = 1e9f;
		float max = 0;
		float sum = 0;
		int count = 0;

		void add(float ms)
		{
			if (ms < min) min = ms;
			if (ms > max) max = ms;
			sum += ms;
			count++;
		}

		float avg() const { return count > 0 ? sum / count : 0; }
	};

public:
	void on_server_timing(uint64_t frame_index, int stream,
		int64_t enc_begin, int64_t enc_end,
		int64_t s_begin, int64_t s_end)
	{
		std::lock_guard lock(mutex);
		frame_timing * rec = find(frame_index, stream);
		if (!rec)
		{
			records.push_back({.frame_index = frame_index, .stream = stream});
			rec = &records.back();
		}
		rec->encode_begin = enc_begin;
		rec->encode_end = enc_end;
		rec->send_begin = s_begin;
		rec->send_end = s_end;
		rec->has_server_timing = true;
		trim();
	}

	void on_shard_received(uint64_t frame_index, int stream, bool is_first, bool is_last)
	{
		int64_t t = now_ns();
		std::lock_guard lock(mutex);
		frame_timing * rec = find(frame_index, stream);
		if (!rec)
		{
			records.push_back({.frame_index = frame_index, .stream = stream});
			rec = &records.back();
		}
		if (is_first)
			rec->recv_first = t;
		if (is_last)
			rec->recv_last = t;
		trim();
	}

	void on_pushed_to_decoder(uint64_t frame_index, int stream)
	{
		int64_t t = now_ns();
		std::lock_guard lock(mutex);
		frame_timing * rec = find(frame_index, stream);
		if (!rec)
		{
			records.push_back({.frame_index = frame_index, .stream = stream});
			rec = &records.back();
		}
		rec->decoder_in = t;
		trim();
	}

	void on_frame_decoded(uint64_t frame_index, int stream)
	{
		int64_t t = now_ns();
		std::lock_guard lock(mutex);
		frame_timing * rec = find(frame_index, stream);
		if (!rec) return;
		rec->decoded = t;
	}

	void on_frame_rendered(uint64_t frame_index, int stream)
	{
		int64_t t = now_ns();
		std::lock_guard lock(mutex);
		frame_timing * rec = find(frame_index, stream);
		if (!rec) return;
		rec->rendered = t;
	}

	void on_frame_submitted(uint64_t frame_index, int stream, int64_t predicted_display)
	{
		int64_t t = now_ns();
		std::lock_guard lock(mutex);
		frame_timing * rec = find(frame_index, stream);
		if (!rec) return;
		rec->submitted = t;
		rec->predicted_display = predicted_display;
		rec->complete = true;

		int count = completed_count.fetch_add(1) + 1;

		if (count <= 10 || (count % SUMMARY_INTERVAL) == 0)
		{
			log_frame(*rec);
		}

		if (count % SUMMARY_INTERVAL == 0)
		{
			log_summary_locked();
		}
	}

	void reset()
	{
		std::lock_guard lock(mutex);
		records.clear();
		completed_count.store(0);
		logged_count.store(0);
	}

private:
	void log_frame(const frame_timing & r)
	{
		if (!r.has_server_timing || !r.recv_first || !r.recv_last ||
			!r.decoder_in || !r.decoded || !r.rendered || !r.submitted)
		{
			spdlog::warn("[LATENCY] frame={} s={} INCOMPLETE (srv={} recv_f={} recv_l={} dec_in={} dec={} rend={} sub={})",
				r.frame_index, r.stream,
				r.has_server_timing, r.recv_first != 0, r.recv_last != 0,
				r.decoder_in != 0, r.decoded != 0, r.rendered != 0, r.submitted != 0);
			return;
		}

		float encode_ms = (r.encode_end - r.encode_begin) / 1e6f;
		float send_ms = (r.send_end - r.send_begin) / 1e6f;
		float network_ms = (r.recv_first - r.send_end) / 1e6f;
		float assembly_ms = (r.recv_last - r.recv_first) / 1e6f;
		float queue_ms = (r.decoder_in - r.recv_last) / 1e6f;
		float decode_ms = (r.decoded - r.decoder_in) / 1e6f;
		float render_wait_ms = (r.rendered - r.decoded) / 1e6f;
		float blit_ms = (r.submitted - r.rendered) / 1e6f;
		float display_wait_ms = (r.predicted_display - r.submitted) / 1e6f;
		float total_ms = (r.predicted_display - r.encode_begin) / 1e6f;

		spdlog::warn("[LATENCY] f={} s={} | TOTAL={:.1f}ms | enc={:.1f} send={:.1f} net={:.1f} asm={:.1f} queue={:.1f} dec={:.1f} rend_wait={:.1f} blit={:.1f} disp_wait={:.1f}",
			r.frame_index, r.stream, total_ms,
			encode_ms, send_ms, network_ms, assembly_ms,
			queue_ms, decode_ms, render_wait_ms,
			blit_ms, display_wait_ms);
	}

	void log_summary_locked()
	{
		stage_stats encode, send, network, assembly, queue, decode, render_wait, blit, display_wait, total;

		for (const auto & r : records)
		{
			if (!r.complete || !r.has_server_timing) continue;
			if (!r.recv_first || !r.recv_last || !r.decoder_in || !r.decoded || !r.rendered || !r.submitted)
				continue;

			encode.add((r.encode_end - r.encode_begin) / 1e6f);
			send.add((r.send_end - r.send_begin) / 1e6f);
			network.add((r.recv_first - r.send_end) / 1e6f);
			assembly.add((r.recv_last - r.recv_first) / 1e6f);
			queue.add((r.decoder_in - r.recv_last) / 1e6f);
			decode.add((r.decoded - r.decoder_in) / 1e6f);
			render_wait.add((r.rendered - r.decoded) / 1e6f);
			blit.add((r.submitted - r.rendered) / 1e6f);
			display_wait.add((r.predicted_display - r.submitted) / 1e6f);
			total.add((r.predicted_display - r.encode_begin) / 1e6f);
		}

		if (total.count == 0)
		{
			spdlog::warn("[LATENCY SUMMARY] no complete frames in window");
			return;
		}

		spdlog::warn("[LATENCY SUMMARY] n={} | TOTAL avg={:.1f} min={:.1f} max={:.1f} | "
			"encode={:.1f} send={:.1f} net={:.1f}(max={:.1f}) asm={:.1f} queue={:.1f} "
			"decode={:.1f}(max={:.1f}) rend_wait={:.1f}(max={:.1f}) blit={:.1f} disp_wait={:.1f}(max={:.1f})",
			total.count,
			total.avg(), total.min, total.max,
			encode.avg(), send.avg(),
			network.avg(), network.max,
			assembly.avg(), queue.avg(),
			decode.avg(), decode.max,
			render_wait.avg(), render_wait.max,
			blit.avg(),
			display_wait.avg(), display_wait.max);
	}
};

extern latency_tracker g_latency;
