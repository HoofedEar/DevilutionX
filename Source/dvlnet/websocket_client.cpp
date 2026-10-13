#include "dvlnet/websocket_client.h"

#include <cctype>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <string_view>

#include <emscripten.h>
#include <emscripten/websocket.h>

#ifdef USE_SDL3
#include <SDL3/SDL_error.h>
#else
#include <SDL.h>
#endif

#include "utils/language.h"
#include "utils/log.hpp"
#include "utils/str_cat.hpp"

// EM_JS declares extern "C" functions, so these live outside the namespaces
EM_JS(int, dvlnet_PageIsSecure, (), {
	return location.protocol === 'https:' ? 1 : 0;
});

// new WebSocket() throws on a malformed URL, which would abort the whole program
EM_JS(int, dvlnet_IsValidWebSocketUrl, (const char *url), {
	try {
		const protocol = new URL(UTF8ToString(url)).protocol;
		return protocol === 'ws:' || protocol === 'wss:' ? 1 : 0;
	} catch (e) {
		return 0;
	}
});

namespace devilution::net {

namespace {

bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix)
{
	if (text.size() < prefix.size())
		return false;
	for (size_t i = 0; i < prefix.size(); i++) {
		if (std::tolower(static_cast<unsigned char>(text[i])) != prefix[i])
			return false;
	}
	return true;
}

/**
 * @brief Accepts a full ws:// or wss:// URL, or "host", "host:port", "host/path", "[::1]:port" etc.,
 * which connect securely when the page itself was loaded securely (browsers require this).
 * Without a port, the standard port is used (443 for wss), which is what a reverse proxy listens on.
 */
std::string MakeUrl(std::string_view address)
{
	if (StartsWithIgnoreCase(address, "ws://") || StartsWithIgnoreCase(address, "wss://"))
		return std::string(address);
	const std::string_view scheme = dvlnet_PageIsSecure() != 0 ? "wss://" : "ws://";
	// A bare IPv6 address needs brackets to be a valid URL
	if (!address.empty() && address[0] != '[' && address.find(':') != address.rfind(':'))
		return StrCat(scheme, "[", address, "]");
	return StrCat(scheme, address);
}

} // namespace

int websocket_client::create(std::string_view /*addrstr*/)
{
	const std::string_view message = _("Games cannot be hosted in the browser. Join a server instead.");
	SDL_SetError("%.*s", static_cast<int>(message.size()), message.data());
	return -1;
}

int websocket_client::join(std::string_view addrstr)
{
	constexpr int MsSleep = 10;
	// Covers DNS, TCP and TLS setup, and a reverse proxy connecting to the server
	constexpr int ConnectTimeoutMs = 10000;
	// Same as tcp_client
	constexpr int JoinTimeoutMs = 2500;

	if (emscripten_websocket_is_supported() == EM_FALSE) {
		SDL_SetError("This browser does not support WebSockets");
		return -1;
	}

	const std::string url = MakeUrl(addrstr);
	if (dvlnet_IsValidWebSocketUrl(url.c_str()) == 0) {
		const std::string message = StrCat(_("Invalid server address"), ": ", url);
		SDL_SetError("%s", message.c_str());
		return -1;
	}

	EmscriptenWebSocketCreateAttributes attributes;
	emscripten_websocket_init_create_attributes(&attributes);
	attributes.url = url.c_str();
	const EMSCRIPTEN_WEBSOCKET_T socket = emscripten_websocket_new(&attributes);
	if (socket <= 0) {
		SDL_SetError("Could not create WebSocket (error %d)", socket);
		return -1;
	}
	socket_ = socket;
	state_ = SocketState::Connecting;
	emscripten_websocket_set_onopen_callback(socket_, this, OnOpen);
	emscripten_websocket_set_onmessage_callback(socket_, this, OnMessage);
	emscripten_websocket_set_onerror_callback(socket_, this, OnError);
	emscripten_websocket_set_onclose_callback(socket_, this, OnClose);

	// The browser connects in the background and reports back through the callbacks,
	// which only run while we yield to it
	for (int waited = 0; state_ == SocketState::Connecting && waited < ConnectTimeoutMs; waited += MsSleep)
		emscripten_sleep(MsSleep);
	if (state_ != SocketState::Open) {
		CloseSocket();
		// Browsers do not say why a WebSocket failed to connect, so neither can we
		const std::string message = StrCat(_("Unable to connect"), " (", url, ")");
		SDL_SetError("%s", message.c_str());
		return -1;
	}

	cookie_self = packet_out::GenerateCookie();
	std::expected<std::unique_ptr<packet>, PacketError> pkt
	    = pktfty->make_packet<PT_JOIN_REQUEST>(
	        PLR_BROADCAST, PLR_MASTER, cookie_self, game_init_info);
	if (!pkt.has_value()) {
		const std::string_view message = pkt.error().what();
		SDL_SetError("make_packet: %.*s", static_cast<int>(message.size()), message.data());
		return -1;
	}
	std::expected<void, PacketError> sendResult = send(**pkt);
	if (!sendResult.has_value()) {
		const std::string_view message = sendResult.error().what();
		SDL_SetError("send: %.*s", static_cast<int>(message.size()), message.data());
		return -1;
	}
	for (int waited = 0; waited < JoinTimeoutMs; waited += MsSleep) {
		std::expected<void, PacketError> pollResult = poll();
		if (!pollResult.has_value()) {
			const std::string_view message = pollResult.error().what();
			SDL_SetError("%.*s", static_cast<int>(message.size()), message.data());
			return -1;
		}
		if (plr_self != PLR_BROADCAST)
			break; // join successful
		emscripten_sleep(MsSleep);
	}
	if (plr_self == PLR_BROADCAST) {
		const std::string_view message = _("Unable to connect");
		SDL_SetError("%.*s", static_cast<int>(message.size()), message.data());
		return -1;
	}

	return plr_self;
}

bool websocket_client::IsGameHost()
{
	return false;
}

std::expected<void, PacketError> websocket_client::poll()
{
	if (failed_)
		return {};
	// Handle everything that arrived before the connection closed, such as the reason we were kicked
	std::expected<void, PacketError> result = ProcessFrames();
	if (!result.has_value()) {
		failed_ = true;
		CloseSocket();
		return result;
	}
	if (state_ == SocketState::Closed && socket_ != 0) {
		failed_ = true;
		CloseSocket();
		return std::unexpected(PacketError(_("Connection to the server was closed")));
	}
	return {};
}

std::expected<void, PacketError> websocket_client::ProcessFrames()
{
	while (true) {
		std::expected<bool, PacketError> ready = recv_queue.PacketReady();
		if (!ready.has_value())
			return std::unexpected(ready.error());
		if (!*ready)
			return {};
		if (recv_queue.ReadPacketFlags() == TcpErrorCodeFlags)
			return std::unexpected(ReadTcpErrorCode());
		std::expected<void, PacketError> result
		    = recv_queue.ReadPacket()
		          .and_then([this](buffer_t &&pktData) { return pktfty->make_packet(pktData); })
		          .and_then([this](std::unique_ptr<packet> &&pkt) { return RecvLocal(*pkt); });
		if (!result.has_value())
			return result;
	}
}

PacketError websocket_client::ReadTcpErrorCode()
{
	std::expected<buffer_t, PacketError> pktResult = recv_queue.ReadPacket();
	if (!pktResult.has_value())
		return pktResult.error();

	const buffer_t &pktData = *pktResult;
	if (pktData.size() != 1)
		return PacketError();

	auto code = static_cast<PacketError::ErrorCode>(pktData[0]);
	if (code == PacketError::ErrorCode::DecryptionFailed)
		return PacketError(_("Server failed to decrypt your packet. Check if you typed the password correctly."));
	if (code == PacketError::ErrorCode::Banned)
		return PacketError(_("You are banned from this server."));
	return PacketError(std::format("Unknown error code received from server: {:#04x}", pktData[0]));
}

std::expected<void, PacketError> websocket_client::send(packet &pkt)
{
	if (state_ != SocketState::Open)
		return std::unexpected(PacketError("Not connected to the server"));
	std::expected<buffer_t, PacketError> frame = frame_queue::MakeFrame(pkt.Data());
	if (!frame.has_value())
		return std::unexpected(frame.error());
	// The browser copies the data, so the frame does not need to outlive this call
	const EMSCRIPTEN_RESULT result = emscripten_websocket_send_binary(socket_, frame->data(), static_cast<uint32_t>(frame->size()));
	if (result != EMSCRIPTEN_RESULT_SUCCESS)
		return std::unexpected(PacketError(std::format("WebSocket send failed (error {})", result)));
	return {};
}

bool websocket_client::SNetLeaveGame(net::leaveinfo_t type)
{
	auto ret = base::SNetLeaveGame(type);
	process_network_packets();
	// Browsers send everything queued before the close, including the leave packet above
	CloseSocket();
	return ret;
}

std::string websocket_client::make_default_gamename()
{
	return "";
}

void websocket_client::CloseSocket()
{
	if (socket_ == 0)
		return;
	if (state_ != SocketState::Closed)
		emscripten_websocket_close(socket_, 1000, "");
	// Also unregisters the callbacks, so they cannot reach this object after it is destroyed
	emscripten_websocket_delete(socket_);
	socket_ = 0;
	state_ = SocketState::Closed;
}

EM_BOOL websocket_client::OnOpen(int /*eventType*/, const EmscriptenWebSocketOpenEvent * /*event*/, void *userData)
{
	auto &client = *static_cast<websocket_client *>(userData);
	client.state_ = SocketState::Open;
	return EM_TRUE;
}

EM_BOOL websocket_client::OnMessage(int /*eventType*/, const EmscriptenWebSocketMessageEvent *event, void *userData)
{
	auto &client = *static_cast<websocket_client *>(userData);
	if (event->isText != EM_FALSE || event->numBytes == 0)
		return EM_TRUE;
	// Parsed later in poll(), on the game's schedule like tcp_client
	client.recv_queue.Write(buffer_t(event->data, event->data + event->numBytes));
	return EM_TRUE;
}

EM_BOOL websocket_client::OnError(int /*eventType*/, const EmscriptenWebSocketErrorEvent * /*event*/, void * /*userData*/)
{
	// Always followed by a close event, which is where the connection is marked as closed
	return EM_TRUE;
}

EM_BOOL websocket_client::OnClose(int /*eventType*/, const EmscriptenWebSocketCloseEvent *event, void *userData)
{
	auto &client = *static_cast<websocket_client *>(userData);
	LogVerbose("WebSocket closed: code {}, reason \"{}\"", event->code, std::string_view(event->reason));
	client.state_ = SocketState::Closed;
	return EM_TRUE;
}

websocket_client::~websocket_client()
{
	CloseSocket();
}

} // namespace devilution::net
