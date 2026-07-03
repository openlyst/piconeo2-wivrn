#pragma once

/*
 * WiVRn session manager for Pico Neo 2.
 * Handles the TCP control and UDP stream sockets, handshake,
 * and packet send/receive for the WiVRn protocol.
 */

#include "crypto.h"
#include "wivrn_packets.h"
#include "wivrn_sockets.h"
#include <chrono>
#include <poll.h>
#include <functional>

using namespace wivrn;

class neo2_handshake_error : public std::exception
{
	std::string msg;

public:
	const char * what() const noexcept override;
	neo2_handshake_error(std::string_view message);
};

class neo2_session
{
public:
	using ctrl_socket = typed_socket<TCP, to_headset::packets, from_headset::packets>;
	using data_socket = typed_socket<UDP, to_headset::packets, from_headset::packets>;

private:
	ctrl_socket control;
	data_socket stream;

	std::atomic<uint64_t> bytes_sent_ = 0;
	std::atomic<uint64_t> bytes_received_ = 0;
	bool handshake_complete = false;

	template <typename T>
	void perform_handshake(T address, bool tcp_only, crypto::key & keypair,
	                        const std::string & device_name,
	                        std::function<std::string(int fd)> pin_provider);

public:
	std::variant<in_addr, in6_addr> remote_address;

	neo2_session(in6_addr address, int port, bool tcp_only,
	             crypto::key & keypair,
	             const std::string & device_name,
	             std::function<std::string(int fd)> pin_provider);
	neo2_session(in_addr address, int port, bool tcp_only,
	             crypto::key & keypair,
	             const std::string & device_name,
	             std::function<std::string(int fd)> pin_provider);
	neo2_session(const neo2_session &) = delete;
	neo2_session & operator=(const neo2_session &) = delete;

	template <typename T>
	void send_control(T && packet)
	{
		bytes_sent_ += control.send(std::move(packet));
	}

	template <typename T>
	void send_stream(T && packet)
	{
		if (stream)
			bytes_sent_ += stream.send(std::move(packet));
		else
			bytes_sent_ += control.send(std::move(packet));
	}

	template <typename T>
	int poll_packets(T && visitor, std::chrono::milliseconds timeout)
	{
		pollfd fds[2] = {};
		fds[0].events = POLLIN;
		fds[0].fd = stream.get_fd();
		fds[1].events = POLLIN;
		fds[1].fd = control.get_fd();

		while (auto packet = stream.receive_pending(&bytes_received_))
			std::visit(std::forward<T>(visitor), std::move(*packet));
		while (auto packet = control.receive_pending(&bytes_received_))
			std::visit(std::forward<T>(visitor), std::move(*packet));

		int r = ::poll(fds, std::size(fds), timeout.count());
		if (r < 0)
			throw std::system_error(errno, std::system_category());

		if (fds[0].revents & (POLLHUP | POLLERR))
			throw std::runtime_error("Stream socket error");

		if (fds[1].revents & (POLLHUP | POLLERR))
			throw std::runtime_error("Control socket error");

		if (fds[0].revents & POLLIN)
		{
			auto packet = stream.receive(&bytes_received_);
			if (packet)
				std::visit(std::forward<T>(visitor), std::move(*packet));
		}

		if (fds[1].revents & POLLIN)
		{
			auto packet = control.receive(&bytes_received_);
			if (packet)
				std::visit(std::forward<T>(visitor), std::move(*packet));
		}

		return r;
	}

	uint64_t bytes_received() const { return bytes_received_; }
	uint64_t bytes_sent() const { return bytes_sent_; }
	bool is_connected() const { return handshake_complete; }
};
