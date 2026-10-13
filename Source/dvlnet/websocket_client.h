#pragma once

#include <expected>
#include <string>
#include <string_view>

#include <emscripten/websocket.h>

#include "dvlnet/base.h"
#include "dvlnet/frame_queue.h"
#include "dvlnet/packet.h"

namespace devilution::net {

/**
 * @brief Client-server multiplayer for the browser build, which cannot open TCP connections.
 *
 * Speaks the same protocol as tcp_client to a dedicated server (DevXServer), with each WebSocket
 * message carrying TCP frames. Browsers cannot listen for connections, so games cannot be hosted.
 */
class websocket_client : public base {
public:
	int create(std::string_view addrstr) override;
	int join(std::string_view addrstr) override;

	std::expected<void, PacketError> poll() override;
	std::expected<void, PacketError> send(packet &pkt) override;

	bool SNetLeaveGame(net::leaveinfo_t type) override;

	~websocket_client() override;

	std::string make_default_gamename() override;

protected:
	bool IsGameHost() override;

private:
	enum class SocketState : uint8_t {
		Connecting,
		Open,
		Closed,
	};

	EMSCRIPTEN_WEBSOCKET_T socket_ = 0;
	SocketState state_ = SocketState::Closed;
	/** @brief Set once an error has been returned from poll(), after which received data is ignored. */
	bool failed_ = false;
	frame_queue recv_queue;

	std::expected<void, PacketError> ProcessFrames();
	PacketError ReadTcpErrorCode();
	void CloseSocket();

	static EM_BOOL OnOpen(int eventType, const EmscriptenWebSocketOpenEvent *event, void *userData);
	static EM_BOOL OnMessage(int eventType, const EmscriptenWebSocketMessageEvent *event, void *userData);
	static EM_BOOL OnError(int eventType, const EmscriptenWebSocketErrorEvent *event, void *userData);
	static EM_BOOL OnClose(int eventType, const EmscriptenWebSocketCloseEvent *event, void *userData);
};

} // namespace devilution::net
