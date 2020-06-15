#include <utility>

#include "wire/SocketWire.h"
// clang-format off
#include "util/fix_ho_spdlog.h"
// clang-format on
#include "spdlog/sinks/stdout_color_sinks-inl.h"

#include <utility>
#include <thread>
#include <csignal>

namespace rd
{
std::shared_ptr<spdlog::logger> SocketWire::Base::logger =
	spdlog::stderr_color_mt<spdlog::synchronous_factory>("wireLog", spdlog::color_mode::automatic);

std::chrono::milliseconds SocketWire::timeout = std::chrono::milliseconds(500);

constexpr int32_t SocketWire::Base::ACK_MESSAGE_LENGTH;
constexpr int32_t SocketWire::Base::PING_MESSAGE_LENGTH;
constexpr int32_t SocketWire::Base::PACKAGE_HEADER_LENGTH;

SocketWire::Base::Base(std::string id, Lifetime lifetime, IScheduler* scheduler)
	: WireBase(scheduler), id(std::move(id)), lifetime(lifetime), scheduler(scheduler), local_send_buffer(SEND_BUFFER_SIZE)
{
	async_send_buffer.pause("initial");
	async_send_buffer.start();
	ping_pkg_header.write_integral(PING_MESSAGE_LENGTH);
}

void SocketWire::Base::receiverProc() const
{
	while (!lifetime->is_terminated())
	{
		try
		{
			if (!socket_provider->IsSocketValid())
			{
				logger->debug("{}: stop receive messages because socket disconnected", this->id);
				//					async_send_buffer.terminate();
				break;
			}

			if (!read_and_dispatch_message())
			{
				logger->debug("{}: connection was gracefully shutdown", id);
				//					async_send_buffer.terminate();
				break;
			}
		}
		catch (std::exception const& ex)
		{
			logger->error("{} caught processing | {}", this->id, ex.what());
			//				async_send_buffer.terminate();
			break;
		}
	}
}

bool SocketWire::Base::send0(Buffer::ByteArray const& msg, sequence_number_t seqn) const
{
	try
	{
		std::lock_guard<decltype(socket_send_lock)> guard(socket_send_lock);

		int32_t msglen = static_cast<int32_t>(msg.size());

		send_package_header.rewind();
		send_package_header.write_integral(msglen);
		send_package_header.write_integral(seqn);

		RD_ASSERT_THROW_MSG(
			socket_provider->Send(send_package_header.data(), send_package_header.get_position()) == PACKAGE_HEADER_LENGTH,
			this->id +
				": failed to send header over the network"
				", reason: " +
				socket_provider->DescribeError())

		RD_ASSERT_THROW_MSG(socket_provider->Send(msg.data(), msglen) == msglen, this->id +
																					 ": failed to send package over the network"
																					 ", reason: " +
																					 socket_provider->DescribeError());
		logger->info("{}: were sent {} bytes", this->id,  msglen);
		//        RD_ASSERT_MSG(socketProvider->Flush(), "{}: failed to flush");
		return true;
	}
	catch (std::exception const& e)
	{
		//			async_send_buffer.pause("send0");
		logger->warn("Send0 failed due to: | {}", e.what());
		return false;
	}
}

void SocketWire::Base::send(RdId const& rd_id, std::function<void(Buffer& buffer)> writer) const
{
	RD_ASSERT_MSG(!rd_id.isNull(), "{}: id mustn't be null");

	local_send_buffer.write_integral<int32_t>(0);	 // placeholder for length

	rd_id.write(local_send_buffer);					 // write id
	local_send_buffer.write_integral<int16_t>(0);	 // placeholder for context
	writer(local_send_buffer);						 // write rest

	int32_t len = static_cast<int32_t>(local_send_buffer.get_position());

	local_send_buffer.rewind();
	local_send_buffer.write_integral<int32_t>(len - 4);
	local_send_buffer.set_position(static_cast<size_t>(len));
	async_send_buffer.put(std::move(local_send_buffer).getRealArray());
	local_send_buffer.rewind();
}

void SocketWire::Base::set_socket_provider(std::shared_ptr<CActiveSocket> new_socket)
{
	{
		std::lock_guard<decltype(socket_send_lock)> guard(socket_send_lock);
		socket_provider = std::move(new_socket);
		socket_send_var.notify_all();
	}
	{
		std::lock_guard<decltype(lock)> guard(lock);
		if (lifetime->is_terminated())
		{
			return;
		}
	}

	auto heartbeat = LifetimeDefinition::use([this](Lifetime heartbeatLifetime) {
		const auto heartbeat = start_heartbeat(heartbeatLifetime).share();

		async_send_buffer.resume();

		connected.set(true);

		receiverProc();

		connected.set(false);

		async_send_buffer.pause("Disconnected");

		return heartbeat;
	});
	const auto status = heartbeat.wait_for(timeout);

	logger->debug("{}: waited for heartbeat to stop with status: {}", this->id, status);

	if (!socket_provider->Shutdown(CSimpleSocket::Both))
	{
		// double close?
		logger->warn("{}: possibly double close after disconnect", this->id);
	}
}

bool SocketWire::Base::connection_established(int32_t timestamp, int32_t notion_timestamp)
{
	return timestamp - notion_timestamp <= MaximumHeartbeatDelay;
}

std::future<void> SocketWire::Base::start_heartbeat(Lifetime lifetime)
{
	return std::async([this, lifetime] {
		while (!lifetime->is_terminated())
		{
			std::this_thread::sleep_for(heartBeatInterval);
			ping();
		}
	});
}

bool SocketWire::Base::read_from_socket(Buffer::word_t* res, int32_t msglen) const
{
	int32_t ptr = 0;
	while (ptr < msglen)
	{
		RD_ASSERT_MSG(hi >= lo, "hi >= lo")

		int32_t rest = msglen - ptr;
		int32_t available = static_cast<int32_t>(hi - lo);

		if (available > 0)
		{
			int32_t copylen = (std::min)(rest, available);
			std::copy(lo, lo + copylen, res + ptr);
			lo += copylen;
			ptr += copylen;
		}
		else
		{
			if (hi == receiver_buffer.end())
			{
				hi = lo = receiver_buffer.begin();
			}
			logger->info("{}: receive started", this->id);
			if (!socket_provider->IsSocketValid())
			{
				return false;
			}
			int32_t read = socket_provider->Receive(static_cast<int32_t>(receiver_buffer.end() - hi), &*hi);
			if (read == -1)
			{
				logger->error("{}: error has occurred while receiving", this->id);
				return false;
			}
			if (read == 0)
			{
				logger->warn("{}: socket was shutted down for receiving", this->id);
				return false;
			}
			hi += read;
			if (read > 0)
			{
				logger->info("{}: receive finished: {} bytes read", this->id, read);
			}
		}
	}
	if (ptr != msglen)
	{
		logger->error("read invalid number of bytes from socket, expected: {}, actual: {}", msglen, ptr);
		assert(false);
	}
	return true;
}

static constexpr std::pair<int, sequence_number_t> INVALID_HEADER = std::make_pair(-1, -1);

std::pair<int, sequence_number_t> SocketWire::Base::read_header() const
{
	int32_t len = 0;
	sequence_number_t seqn = 0;
	while (true)
	{
		if (!read_integral_from_socket(len))
		{
			return INVALID_HEADER;
		}
		if (len == PING_MESSAGE_LENGTH)
		{
			int32_t received_timestamp = 0;
			int32_t received_counterpart_timestamp = 0;
			if (!read_integral_from_socket(received_timestamp))
			{
				return INVALID_HEADER;
			}
			if (!read_integral_from_socket(received_counterpart_timestamp))
			{
				return INVALID_HEADER;
			}

			counterpart_timestamp = received_timestamp;
			counterpart_acknowledge_timestamp = received_counterpart_timestamp;

			if ((connection_established(current_timestamp, counterpart_acknowledge_timestamp)))
			{
				if (!heartbeatAlive.get())
				{	 // only on change
					logger->trace(
						"Connection is alive after receiving PING {}: "
						"received_timestamp: {}, "
						"received_counterpart_timestamp: {}, "
						"current_timestamp: {}, "
						"counterpart_timestamp: {}, "
						"counterpart_acknowledge_timestamp: {}, ",
						id, received_timestamp, received_counterpart_timestamp, current_timestamp, counterpart_timestamp,
						counterpart_acknowledge_timestamp);
				}
				heartbeatAlive.set(true);
			}
			continue;
		}
		if (!read_integral_from_socket(seqn))
		{
			return INVALID_HEADER;
		}

		if (len == ACK_MESSAGE_LENGTH)
		{
			async_send_buffer.acknowledge(seqn);
			continue;
		}
		return std::make_pair(len, seqn);
	}
}

int32_t SocketWire::Base::read_package() const
{
	receive_pkg.rewind();

	const auto pair = read_header();
	if (pair == INVALID_HEADER)
	{
		logger->debug("{}: failed to read header", this->id);
		return -1;
	}
	const auto len = pair.first;
	const auto seqn = pair.second;

	logger->debug("{}: read len={}, seqn={}, max_received_seqn={}", this->id, len, seqn, max_received_seqn);

	receive_pkg.require_available(len);
	if (!read_data_from_socket(receive_pkg.data(), len))
	{
		logger->debug("{}: failed to read package", this->id);
		return -1;
	}
	send_ack(seqn);
	if (seqn <= max_received_seqn && seqn != 1)
	{
		return true;
	}
	max_received_seqn = seqn;

	logger->info("{}: was received package, bytes={}, seqn={}", this->id, len, seqn);
	return len;
}

bool SocketWire::Base::read_and_dispatch_message() const
{
	sz = (sz == -1 ? receive_pkg.read_integral<int32_t>() : sz);
	if (sz == -1)
	{
		logger->error("sz == -1");
		return false;
	}
	id_ = (id_ == -1 ? receive_pkg.read_integral<RdId::hash_t>() : id_);
	if (id_ == -1)
	{
		logger->error("id == -1");
		return false;
	}
	logger->trace("{}: message info: sz={}, id={}", this->id, sz, id_);
	const RdId rd_id{id_};
	sz -= 8;	// RdId
	message.require_available(sz);

	if (!receive_pkg.read(message.data() + message.get_position(), sz - message.get_position()))
	{
		logger->error("{}: constructing message failed", this->id);
		return false;
	}

	logger->debug("{}: message received", this->id);
	message_broker.dispatch(rd_id, std::move(message));
	logger->debug("{}: message dispatched", this->id);

	sz = -1;
	id_ = -1;
	message.rewind();
	return true;
	//		RD_ASSERT_MSG(summary_size == sz, "Broken message, read:%d bytes, expected:%d bytes", summary_size, sz)
}

CSimpleSocket* SocketWire::Base::get_socket_provider() const
{
	return socket_provider.get();
}

void SocketWire::Base::ping() const
{
	if (!connection_established(current_timestamp, counterpart_acknowledge_timestamp))
	{
		if (heartbeatAlive.get())
		{	 // only on change
			logger->trace(
				"Disconnect detected while sending PING {}: "
				"current_timestamp: {}, "
				"counterpart_timestamp: {}, "
				"counterpart_acknowledge_timestamp: {}",
				this->id, current_timestamp, counterpart_timestamp, counterpart_acknowledge_timestamp);
		}
		heartbeatAlive.set(false);
	}
	try
	{
		ping_pkg_header.set_position(sizeof(PING_MESSAGE_LENGTH));
		ping_pkg_header.write_integral(current_timestamp);
		ping_pkg_header.write_integral(counterpart_timestamp);
		{
			std::lock_guard<decltype(socket_send_lock)> guard(socket_send_lock);
			RD_ASSERT_THROW_MSG(
				socket_provider->Send(ping_pkg_header.data(), ping_pkg_header.get_position()) == PACKAGE_HEADER_LENGTH,
				this->id +
					": failed to send ping over the network"
					", reason: " +
					socket_provider->DescribeError())
		}

		++current_timestamp;
	}
	catch (std::exception const& e)
	{
		logger->warn("{}: exception raised during PING | {}", this->id, e.what());
	}
}

bool SocketWire::Base::send_ack(sequence_number_t seqn) const
{
	logger->trace("{} send ack {}", id, seqn);
	try
	{
		ack_buffer.rewind();
		ack_buffer.write_integral(ACK_MESSAGE_LENGTH);
		ack_buffer.write_integral(seqn);
		{
			std::lock_guard<decltype(socket_send_lock)> guard(socket_send_lock);
			RD_ASSERT_THROW_MSG(socket_provider->Send(ack_buffer.data(), ack_buffer.get_position()) == PACKAGE_HEADER_LENGTH,
				this->id +
					": failed to send ack over the network"
					", reason: " +
					socket_provider->DescribeError())
		}
		return true;
	}
	catch (std::exception const& e)
	{
		logger->warn("{}: exception raised during ACK, seqn = {} | {}", id, seqn, e.what());
		return false;
	}
}

SocketWire::Client::Client(Lifetime lifetime, IScheduler* scheduler, uint16_t port, const std::string& id)
	: Base(id, lifetime, scheduler), port(port)
{
	thread = std::thread([this, lifetime]() mutable {
		try
		{
			while (!lifetime->is_terminated())
			{
				try
				{
					socket = std::make_shared<CActiveSocket>();
					RD_ASSERT_THROW_MSG(socket->Initialize(), this->id +
																  ": failed to init ActiveSocket"
																  ", reason: " +
																  socket->DescribeError());
					RD_ASSERT_THROW_MSG(socket->DisableNagleAlgoritm(), this->id +
																			": failed to DisableNagleAlgoritm"
																			", reason: " +
																			socket->DescribeError());

					// On windows connect will try to send SYN 3 times with interval of 500ms (total time is 1second)
					// Connect timeout doesn't work if it's more than 1 second. But we don't need it because we can close socket any
					// moment.

					// https://stackoverflow.com/questions/22417228/prevent-tcp-socket-connection-retries
					// HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\TcpMaxConnectRetransmissions
					logger->info("{}: connecting 127.0.0.1: {}", this->id, this->port);
					RD_ASSERT_THROW_MSG(socket->Open("127.0.0.1", this->port), this->id +
																				   ": failed to open ActiveSocket"
																				   ", reason: " +
																				   socket->DescribeError());
					{
						std::lock_guard<decltype(lock)> guard(lock);
						if (lifetime->is_terminated())
						{
							if (!socket->Close())
							{
								logger->error("{} failed to close socket, reason: {}", this->id, socket->DescribeError());
							}
							return;
						}
					}

					set_socket_provider(socket);
				}
				catch (std::exception const& e)
				{
					std::lock_guard<decltype(lock)> guard(lock);
					bool should_reconnect = false;
					if (!lifetime->is_terminated())
					{
						cv.wait_for(lock, timeout);
						should_reconnect = !lifetime->is_terminated();
					}
					if (should_reconnect)
					{
						continue;
					}
					break;
				}
			}
		}
		catch (std::exception const& e)
		{
			logger->info("{}: closed with exception: {}", this->id, e.what());
		}
		logger->debug("{}: thread expired", this->id);
	});

	lifetime->add_action([this]() {
		logger->info("{}: starts terminating lifetime", this->id);

		const bool send_buffer_stopped = async_send_buffer.stop(timeout);
		logger->debug("{}: send buffer stopped, success: {}", this->id, send_buffer_stopped);

		{
			std::lock_guard<decltype(lock)> guard(lock);
			logger->debug("{}: closing socket", this->id);

			if (socket != nullptr)
			{
				if (!socket->Close())
				{
					logger->error("{}: failed to close socket", this->id);
				}
			}
		}
		cv.notify_all();

		logger->debug("{}: waiting for receiver thread", this->id);
		logger->debug("{}: is thread joinable? {}", this->id, thread.joinable());
		thread.join();
		logger->info("{}: termination finished", this->id);
	});
}

SocketWire::Server::Server(Lifetime lifetime, IScheduler* scheduler, uint16_t port, const std::string& id)
	: Base(id, lifetime, scheduler)
{
#ifdef SIGPIPE
	signal(SIGPIPE, SIG_IGN);
#endif
	RD_ASSERT_MSG(ss->Initialize(), this->id +
										": failed to initialize socket"
										", reason: " +
										socket->DescribeError())
	RD_ASSERT_MSG(ss->Listen("127.0.0.1", port),
		"{}: failed to listen socket on port:" + std::to_string(port) + ", reason: " + ss->DescribeError());

	logger->info("{}: listening 127.0.0.1/{}", this->id, port);
	this->port = ss->GetServerPort();
	RD_ASSERT_MSG(this->port != 0, "{}: port wasn't chosen")

	thread = std::thread([this, lifetime]() mutable {
		while (!lifetime->is_terminated())
		{
			try
			{
				logger->info("{}: accepting started", this->id);
				CActiveSocket* accepted = ss->Accept();
				RD_ASSERT_THROW_MSG(accepted != nullptr, std::string(ss->DescribeError()) + ", reason: " + ss->DescribeError())
				socket.reset(accepted);
				logger->info("{}: accepted passive socket", this->id);
				RD_ASSERT_THROW_MSG(socket->DisableNagleAlgoritm(), this->id +
																		": tcpNoDelay failed"
																		", reason: " +
																		socket->DescribeError())

				{
					std::lock_guard<decltype(lock)> guard(lock);
					if (lifetime->is_terminated())
					{
						logger->debug("{}: closing passive socket", this->id);
						if (!socket->Close())
						{
							logger->error("{}: failed to close socket", this->id);
						}
						logger->info("{}: close passive socket", this->id);
					}
				}

				logger->debug("{}: setting socket provider", this->id);
				set_socket_provider(socket);
			}
			catch (std::exception const& e)
			{
				logger->info("{}: closed with exception: {}", this->id, e.what());
			}
		}
		logger->debug("{}: thread expired", this->id);
	});

	lifetime->add_action([this] {
		logger->info("{}: start terminating lifetime", this->id);

		const bool send_buffer_stopped = async_send_buffer.stop(timeout);
		logger->debug("{}: send buffer stopped, success: {}", this->id, send_buffer_stopped);

		logger->debug("{}: closing server socket", this->id);
		if (!ss->Close())
		{
			logger->error("{}: failed to close server socket", this->id);
		}

		{
			std::lock_guard<decltype(lock)> guard(lock);
			logger->debug("{}: closing socket", this->id);
			if (socket != nullptr)
			{
				if (!socket->Close())
				{
					logger->error("{}: failed to close socket", this->id);
				}
			}
		}

		logger->debug("{}: waiting for receiver thread", this->id);
		logger->debug("{}: is thread joinable? {}", this->id, thread.joinable());
		thread.join();
		logger->info("{}: termination finished", this->id);
	});
}
}	 // namespace rd
