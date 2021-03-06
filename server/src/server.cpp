#include <cstring>

#include <iostream>
#include <chrono>
#include <atomic>
#include <vector>
#include <functional>
#include <type_traits>
#include <stdexcept>
#include <thread>

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/idl.h>
#include <flatbuffers/util.h>

#include <spdlog/spdlog.h>

#include <concurrentqueue.h>

#include "store.h"
#include "libenet.h"
#include "signal_handlers.h"

#include "flightctrlstate_generated.h"
#include "netmessages_generated.h"
#include "server_generated.h"


using msg_handler_t = std::function<void(const keron::net::event_t &, const keron::messages::NetMessage &)>;
using config_ptr = std::unique_ptr<const keron::server::Configuration>;

template<typename T>
using queue_t = moodycamel::ConcurrentQueue<T>;

queue_t<keron::net::outgoing_t> outgoings;

std::shared_ptr<spdlog::logger> logger;

void msg_none(const keron::net::event_t &, const keron::messages::NetMessage &)
{
	logger->info("No message.");
}

void msg_chat(const keron::net::event_t &event, const keron::messages::NetMessage &msg)
{
	auto chat = reinterpret_cast<const keron::messages::Chat *>(msg.message());
	logger->info("Chat message: {}", chat->message()->c_str());
	keron::net::packet_t response(event.packet->data, event.packet->dataLength, event.packet->flags);
	keron::net::outgoing_t out{nullptr, std::move(response), nullptr, event.channelID};
	outgoings.enqueue(std::move(out));
}

void msg_flightctrl(const keron::net::event_t &event, const keron::messages::NetMessage &flight)
{
	auto flightCtrl = reinterpret_cast<const keron::messages::FlightCtrl *>(flight.message());
	logger->info("Flight control state");
}

void msg_clocksync(const keron::net::event_t &event, const keron::messages::NetMessage &msg)
{
	using keron::messages::CreateNetMessage;
	using keron::messages::NetID_ClockSync;
	using keron::messages::CreateClockSync;
	using keron::messages::FinishNetMessageBuffer;

	auto clocksync = reinterpret_cast<const keron::messages::ClockSync *>(msg.message());
	auto now = std::chrono::system_clock::now().time_since_epoch();
	auto server_ts = std::chrono::duration_cast<std::chrono::seconds>(now).count();
	auto client_ts = clocksync->clientTransmission();

	flatbuffers::FlatBufferBuilder fbb;
	auto replysync = CreateNetMessage(fbb, NetID_ClockSync, CreateClockSync(fbb, client_ts, server_ts).Union());

	FinishNetMessageBuffer(fbb, replysync);

	logger->info("Client TS: {}. Server TS: {}", client_ts, server_ts);
	keron::net::packet_t response(fbb.GetBufferPointer(), fbb.GetSize(), event.packet->flags);
	keron::net::outgoing_t out{event.peer, std::move(response), static_cast<int *>(event.peer->data), event.channelID};
	outgoings.enqueue(std::move(out));
}

void load_configuration(flatbuffers::Parser &parser, const std::string &schema, const std::string &configfile)
{
	std::string serverschema;
	std::string configjson;
    parser.builder_.Clear();

	if (!flatbuffers::LoadFile(schema.c_str(), false, &serverschema))
                throw std::runtime_error("Cannot load server schema.");

	parser.Parse(serverschema.c_str());

	if (!flatbuffers::LoadFile(configfile.c_str(), false, &configjson)) {
		spdlog::get("config")->warn() << "No server configuration found. Creating a default one.";
		flatbuffers::FlatBufferBuilder fbb;
		auto cfg = keron::server::CreateConfiguration(fbb,
				fbb.CreateString("*"),
				('K' << 8) | ('S' << 4) | 'P', 8, fbb.CreateString("server.db"), fbb.CreateString("logs/keron.log"));
		auto generator = flatbuffers::GeneratorOptions();
		generator.strict_json = true;
		FinishConfigurationBuffer(fbb, cfg);
		flatbuffers::GenerateText(parser, fbb.GetBufferPointer(), generator, &configjson);

		if (!flatbuffers::SaveFile(configfile.c_str(), configjson.c_str(), configjson.size(), false))
                        throw std::runtime_error("Unable to write default configuration!");

                throw std::runtime_error(
			"A default configuration has been written. "
			"Check the content of `server.json`, and restart the server.");
	}

	parser.Parse(configjson.c_str());
}

std::vector<msg_handler_t> initialize_messages_handlers()
{
	using namespace keron::messages;

	std::vector<msg_handler_t> handlers(NetID_MAXNETID);
	handlers[NetID_NONE] = msg_none;
	handlers[NetID_Chat] = msg_chat;
	handlers[NetID_FlightCtrl] = msg_flightctrl;
	handlers[NetID_ClockSync] = msg_clocksync;

	return handlers;
}

ENetAddress initialize_server_address(const keron::server::Configuration &config)
{
	const std::string host(config.address()->c_str());
	uint16_t port{config.port()};

	ENetAddress address;

	if (host == "*")
		address.host = ENET_HOST_ANY;
	else
		enet_address_set_host(&address, host.c_str());

	address.port = port;
	return address;
}

int main(int argc, char *argv[])
{
	flatbuffers::Parser parser;

	{
		auto config = spdlog::stderr_logger_st("config");
		load_configuration(parser, "schemas/server.fbs", "server.json");
		spdlog::drop(config->name());
	}

	spdlog::set_async_mode();
	auto settings = keron::server::GetConfiguration(parser.builder_.GetBufferPointer());
	logger = spdlog::rotating_logger_mt("log", settings->logs()->c_str(), 5UL * 1024UL * 1024UL, 5);
	keron::server::register_signal_handlers();

	logger->info() << "Firing up storage.";
	keron::db::store_t datastore(settings->datastore()->c_str());

	logger->info() << "Preparing message handlers.";
	auto handlers = initialize_messages_handlers();

	logger->info() << "Initializing network.";
	keron::net::library_t enet;

	auto address = initialize_server_address(*settings);
	keron::net::host_t host(address, settings->maxclients(), 2);
	if (!host) {
		logger->error() << "Creating host.";
		return -3;
	}

	keron::net::event_t event;

	logger->info("Listening on {}:{} with {} clients allowed.",
		settings->address()->c_str(), settings->port(), settings->maxclients());

	logger->info() << "Setting up workers.";
	auto count = settings->concurrency() ? settings->concurrency() : std::max(static_cast<unsigned>(1), std::thread::hardware_concurrency() - 1);
	logger->info("Spawning {} worker(s).", count);
	std::vector<std::thread> workers;
	workers.reserve(count);
	queue_t<keron::net::incoming_t> incomings;

	while (count--) {
		workers.emplace_back(std::move([&handlers, &incomings] {
			auto thread_logger = spdlog::get("log");
			thread_logger->info("Worker {} started.", std::this_thread::get_id());

			while (!keron::server::stop) {
				keron::net::incoming_t msg;

				while (incomings.try_dequeue(msg)) {
					auto &event = msg.event;
					keron::net::packet_t packet(event.packet);
					keron::net::address_t address(event.peer->address);
					thread_logger->debug("Received packet from {} on channel {} size {}B", address.ip(), event.channelID, packet.length());

					flatbuffers::Verifier verifier(packet.data(), packet.length());

					if (!keron::messages::VerifyNetMessageBuffer(verifier)) {
						thread_logger->warn("Incorrect buffer received.");
						break;
					}

					auto message = keron::messages::GetNetMessage(packet.data());
					keron::messages::NetID id = message->message_type();
					thread_logger->debug("Message is: {} {}", id, keron::messages::EnumNameNetID(id));

					if (!(id < handlers.size())) {
						thread_logger->error("No available handlers for message ID {}", id);
						break;
					}

					handlers.at(id)(event, *message);
				}

				// Avoid spinning wildly.
				std::this_thread::sleep_for(std::chrono::milliseconds{25});
			}
			thread_logger->info("Worker {} shutting down.", std::this_thread::get_id());
		}));
	}



	// Main thread: Handle inputs/outputs.
	// enet recommands servicing at least every 50ms
	const auto delay = std::chrono::milliseconds(25);
	while (!keron::server::stop) {
		if (host.service(event, delay.count()) < 0) {
			logger->error("Servicing host.");
			throw std::runtime_error("host servicing");
		}

		// Process incoming event (if any)
		switch (event.type) {
			case ENET_EVENT_TYPE_RECEIVE:
			{
				// Enqueue event for processing.
				keron::net::incoming_t incoming{static_cast<int *>(event.peer->data), event};
				incomings.enqueue(std::move(incoming));
			}
				break;
			case ENET_EVENT_TYPE_CONNECT:
			{
				keron::net::address_t address(event.peer->address);
				logger->info("Connection from: {}", address.ip());
				if (event.data)
				{
					auto clientID = event.data;
					logger->info("Client ID {}", clientID);
					auto txn = datastore.transaction();
					auto dbi = lmdb::dbi::open(txn, nullptr);
					auto cursor = lmdb::cursor::open(txn, dbi);
					std::uint8_t count{0};
					auto key = keron::db::make_val(clientID);
					auto value = keron::db::make_val(count);
					if (cursor.get(key, value, MDB_FIRST))
					{
						count = *value.data<std::uint8_t>();
						logger->info("Last client count: {:d}", count);
					}
					else
					{
						logger->info("First connection.");
					}
					cursor.close();
					++count;
					value = std::move(keron::db::make_val(count));
					dbi.put(txn, key, value);
					txn.commit();
				}

			}
				break;
			case ENET_EVENT_TYPE_DISCONNECT:
			{
				keron::net::address_t address(event.peer->address);
				logger->info("Disconnection from: {}", address.ip());
				// On disconnection, update the generation to avoid ABA issues.
				event.peer->data = static_cast<int *>(event.peer->data) + 1;
			}
				break;
			case ENET_EVENT_TYPE_NONE:
				// reached timeout without incomings.
				break;
			default:
				logger->error("Unhandled event {}", event.type);
		}

		// Process outgoing events.
		keron::net::outgoing_t packet;
		while (outgoings.try_dequeue(packet)) {
			if (packet.peer == nullptr) {
				// Broadcast event.
				host.broadcast(packet.channelID, std::move(packet.payload));
			}
			else {
				// A client may disconnect and its structure be reused.
				// Check that they still live in the same generation.
				if (static_cast<int *>(packet.peer->data) == packet.generation) {
					enet_peer_send(packet.peer, packet.channelID, packet.payload.release());
				}
			}
		}

		logger->flush();
	}

	logger->info("Waiting on workers.");
	logger->flush();
	for (auto &worker: workers)
		worker.join();

	logger->info("Server is shutting down.");
	logger->flush();
	spdlog::drop_all();
	return 0;
}

// vim: shiftwidth=4 tabstop=4
