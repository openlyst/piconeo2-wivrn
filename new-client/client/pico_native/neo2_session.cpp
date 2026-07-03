/*
 * WiVRn session implementation for Pico Neo 2.
 * Adapted from the WiVRn protocol handshake logic.
 */

#include "neo2_session.h"
#include "protocol_version.h"
#include "secrets.h"
#include "smp.h"
#include "wivrn_packets.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/ip.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

#ifndef IPTOS_DSCP_EF
#define IPTOS_DSCP_EF 0xb8
#endif

using namespace std::chrono_literals;

const char * neo2_handshake_error::what() const noexcept
{
	return msg.c_str();
}

neo2_handshake_error::neo2_handshake_error(std::string_view message) :
        msg(message) {}

namespace
{
template <typename T>
void setup_stream_buffers(T & sock)
{
	sock.set_receive_buffer_size(1024 * 1024 * 5);
}
} // namespace

template <typename T>
void neo2_session::perform_handshake(T address, bool tcp_only, crypto::key & keypair,
                                     const std::string & device_name,
                                     std::function<std::string(int fd)> pin_provider)
{
	try
	{
		pollfd fds{};
		fds.events = POLLIN;
		fds.fd = control.get_fd();

		auto recv_packet = [&](std::optional<std::chrono::seconds> timeout = std::nullopt) {
			std::chrono::steady_clock::time_point deadline{};
			if (timeout)
				deadline = std::chrono::steady_clock::now() + *timeout;

			while (true)
			{
				auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
				int r = ::poll(&fds, 1, std::max<int>(ms.count(), 100));
				if (r < 0)
					throw std::system_error(errno, std::system_category());

				if (r > 0 && (fds.revents & POLLIN))
				{
					auto packet = control.receive();
					if (not packet)
						continue;
					return std::move(*packet);
				}

				if (fds.revents & (POLLHUP | POLLERR))
					throw std::runtime_error("Socket error during handshake");

				if (std::chrono::steady_clock::now() >= deadline)
					throw std::runtime_error("Handshake timeout");
			}
		};

		spdlog::info("handshake: sending crypto handshake");
		send_control(from_headset::crypto_handshake{
		        .protocol_version = wivrn::protocol_version,
		        .public_key = keypair.public_key(),
		        .name = device_name,
		});

		spdlog::info("handshake: waiting for server response");
		to_headset::crypto_handshake crypto_reply = std::get<to_headset::crypto_handshake>(recv_packet(10s));
		spdlog::info("handshake: server state={}", (int)crypto_reply.state);

		std::string pin = "000000";
		switch (crypto_reply.state)
		{
			case to_headset::crypto_handshake::crypto_state::encryption_disabled: {
				spdlog::info("handshake: encryption disabled");
				send_control(from_headset::crypto_handshake{});
				to_headset::handshake h{std::get<to_headset::handshake>(recv_packet(10s))};
				spdlog::info("handshake: stream_port={}", h.stream_port);
				if (h.stream_port > 0 && !tcp_only)
				{
					stream = decltype(stream)();
					stream.connect(address, h.stream_port);
					setup_stream_buffers(stream);
				}
				break;
			}

			case to_headset::crypto_handshake::crypto_state::pin_needed:
				pin = pin_provider(control.get_fd());

				try
				{
					crypto::smp pin_check;
					auto msg1 = pin_check.step1(pin);
					send_control(from_headset::pin_check_1{msg1});
					auto msg2 = std::get<to_headset::pin_check_2>(recv_packet(10s)).message;
					auto msg3 = pin_check.step3(msg2);
					send_control(from_headset::pin_check_3{msg3});
					auto msg4 = std::get<to_headset::pin_check_4>(recv_packet(10s)).message;
					if (not pin_check.step5(msg4))
						throw std::runtime_error("Incorrect PIN");
				}
				catch (crypto::smp_cheated &)
				{
					throw std::runtime_error("PIN check failed");
				}
				[[fallthrough]];

			case to_headset::crypto_handshake::crypto_state::client_already_paired: {
				spdlog::info("handshake: using pin \"{}\"", pin);
				crypto::key server_key = crypto::key::from_public_key(crypto_reply.public_key);
				secrets s{keypair, server_key, pin};
				control.set_aes_key_and_ivs(s.control_key, s.control_iv_to_headset, s.control_iv_from_headset);
				send_control(from_headset::crypto_handshake{});
				to_headset::handshake h{std::get<to_headset::handshake>(recv_packet(10s))};
				if (h.stream_port > 0 && !tcp_only)
				{
					stream = decltype(stream)();
					stream.set_aes_key_and_ivs(s.stream_key, s.stream_iv_header_to_headset, s.stream_iv_header_from_headset);
					stream.connect(address, h.stream_port);
					setup_stream_buffers(stream);
				}
				break;
			}

			case to_headset::crypto_handshake::crypto_state::pairing_disabled:
				spdlog::error("handshake: pairing disabled on server");
				return;

			case to_headset::crypto_handshake::crypto_state::incompatible_version:
				spdlog::error("handshake: incompatible protocol version");
				return;
		}

		spdlog::info("handshake: sending stream handshake");
		send_stream(from_headset::handshake{});

		auto deadline = std::chrono::steady_clock::now() + 10s;
		while (true)
		{
			if (poll_packets([](const auto && packet) {
			        return std::is_same_v<std::remove_cvref_t<decltype(packet)>, to_headset::handshake>;
			    }, 100ms))
			{
				handshake_complete = true;
				spdlog::info("handshake: complete");
				return;
			}

			if (std::chrono::steady_clock::now() >= deadline)
				return;

			if (stream)
				stream.send(from_headset::handshake{});
		}
	}
	catch (...)
	{
		spdlog::error("handshake: failed");
	}
}

neo2_session::neo2_session(in6_addr address, int port, bool tcp_only,
                           crypto::key & keypair,
                           const std::string & device_name,
                           std::function<std::string(int fd)> pin_provider) :
        control(address, port), stream(-1), remote_address(address)
{
	perform_handshake(address, tcp_only, keypair, device_name, pin_provider);
}

neo2_session::neo2_session(in_addr address, int port, bool tcp_only,
                           crypto::key & keypair,
                           const std::string & device_name,
                           std::function<std::string(int fd)> pin_provider) :
        control(address, port), stream(-1), remote_address(address)
{
	perform_handshake(address, tcp_only, keypair, device_name, pin_provider);
}
