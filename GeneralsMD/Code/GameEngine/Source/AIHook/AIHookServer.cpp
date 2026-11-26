/*
**	zh-mcp AI hook for Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 zh-mcp contributors
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// AIHookServer.cpp ///////////////////////////////////////////////////////////////////////////////
// Winsock2 server thread for the AI hook (protocol §1). One background thread, select() based,
// bound to 127.0.0.1 only. It only frames lines and moves them through locked queues; it never
// touches game state. The one exception is the "hello" line, built from values the main thread
// publishes through atomics, so hello goes out immediately even while a map is loading.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "AIHook/AIHookInternal.h"

#pragma comment(lib, "ws2_32.lib")

namespace AIHook
{

namespace
{
	const size_t MAX_PENDING_OUT = 256u * 1024u * 1024u;	///< drop a client that stops reading

	std::mutex s_mutex;													///< guards everything below
	std::deque<Inbound> s_inbox;
	std::string s_outbox;												///< bytes waiting to be written to the client
	unsigned s_gen = 0;													///< current connection generation
	bool s_hasClient = false;
	bool s_dropRequested = false;

	std::atomic<bool> s_inGame(false);
	std::atomic<unsigned> s_frame(0);
	std::string s_build;

	SOCKET s_listen = INVALID_SOCKET;

	std::string jsonLine(const Json& j)
	{
		std::string s = j.dump(-1, ' ', false, Json::error_handler_t::replace);
		s.push_back('\n');
		return s;
	}

	std::string helloLine()
	{
		Json data;
		data["protocol"] = (int)PROTOCOL_VERSION;
		data["game"] = "GeneralsMD";
		data["build"] = s_build;
		data["in_game"] = s_inGame.load();
		Json ev;
		ev["event"] = "hello";
		ev["frame"] = s_frame.load();
		ev["data"] = data;
		return jsonLine(ev);
	}

	std::string busyLine()
	{
		Json data;
		data["code"] = "BUSY";
		data["message"] = "another client is connected";
		Json ev;
		ev["event"] = "error";
		ev["frame"] = s_frame.load();
		ev["data"] = data;
		return jsonLine(ev);
	}

	void setNonBlocking(SOCKET s)
	{
		u_long mode = 1;
		ioctlsocket(s, FIONBIO, &mode);
	}

	/// Best-effort short blocking write, used only for the BUSY notice to a rejected connection.
	void sendAllBlocking(SOCKET s, const std::string& data)
	{
		size_t off = 0;
		while (off < data.size())
		{
			int n = ::send(s, data.data() + off, (int)(data.size() - off), 0);
			if (n <= 0)
				return;
			off += (size_t)n;
		}
	}

	void threadMain()
	{
		SOCKET client = INVALID_SOCKET;
		std::string inbuf;
		bool discardingOverlong = false;
		unsigned myGen = 0;
		std::vector<char> rbuf(64 * 1024);

		for (;;)
		{
			// drop request from the main thread (or our own error handling)
			{
				std::lock_guard<std::mutex> lock(s_mutex);
				if (client != INVALID_SOCKET && s_dropRequested)
				{
					closesocket(client);
					client = INVALID_SOCKET;
					s_hasClient = false;
					s_outbox.clear();
					Inbound ev = { Inbound::DISCONNECTED, myGen, std::string() };
					s_inbox.push_back(ev);
				}
				s_dropRequested = false;
			}

			fd_set rset, wset;
			FD_ZERO(&rset);
			FD_ZERO(&wset);
			FD_SET(s_listen, &rset);
			if (client != INVALID_SOCKET)
			{
				FD_SET(client, &rset);
				std::lock_guard<std::mutex> lock(s_mutex);
				if (!s_outbox.empty())
					FD_SET(client, &wset);
			}

			timeval tv;
			tv.tv_sec = 0;
			tv.tv_usec = 5000;	// 5 ms: bounds the latency of lines queued by the main thread
			int ready = ::select(0, &rset, &wset, nullptr, &tv);
			if (ready == SOCKET_ERROR)
			{
				Sleep(10);
				continue;
			}

			// new connections
			if (FD_ISSET(s_listen, &rset))
			{
				SOCKET s = ::accept(s_listen, nullptr, nullptr);
				if (s != INVALID_SOCKET)
				{
					if (client != INVALID_SOCKET)
					{
						sendAllBlocking(s, busyLine());
						shutdown(s, SD_BOTH);
						closesocket(s);
					}
					else
					{
						setNonBlocking(s);
						BOOL nodelay = TRUE;
						setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
						client = s;
						inbuf.clear();
						discardingOverlong = false;
						std::lock_guard<std::mutex> lock(s_mutex);
						myGen = ++s_gen;
						s_hasClient = true;
						s_outbox = helloLine();		// hello is always the first line
						Inbound ev = { Inbound::CONNECTED, myGen, std::string() };
						s_inbox.push_back(ev);
					}
				}
			}

			if (client == INVALID_SOCKET)
				continue;

			bool drop = false;

			// read
			if (FD_ISSET(client, &rset))
			{
				int n = ::recv(client, rbuf.data(), (int)rbuf.size(), 0);
				if (n == 0)
				{
					drop = true;
				}
				else if (n == SOCKET_ERROR)
				{
					if (WSAGetLastError() != WSAEWOULDBLOCK)
						drop = true;
				}
				else
				{
					std::vector<Inbound> lines;
					size_t start = 0;
					for (int i = 0; i < n; ++i)
					{
						if (rbuf[i] != '\n')
							continue;
						if (!discardingOverlong)
						{
							inbuf.append(rbuf.data() + start, (size_t)i - start);
							if (!inbuf.empty() && inbuf[inbuf.size() - 1] == '\r')
								inbuf.erase(inbuf.size() - 1);
							if (!inbuf.empty())
							{
								Inbound ev = { Inbound::LINE, myGen, inbuf };
								lines.push_back(ev);
							}
						}
						inbuf.clear();
						discardingOverlong = false;
						start = (size_t)i + 1;
					}
					if (!discardingOverlong && start < (size_t)n)
						inbuf.append(rbuf.data() + start, (size_t)n - start);
					if (!discardingOverlong && inbuf.size() > (size_t)MAX_LINE_BYTES)
					{
						// protocol §1: max line length 4 MiB. Tell the client and skip to the next newline.
						inbuf.clear();
						discardingOverlong = true;
						Json data;
						data["code"] = "BAD_ARGS";
						data["message"] = "line exceeds 4 MiB";
						Json ev;
						ev["event"] = "error";
						ev["frame"] = s_frame.load();
						ev["data"] = data;
						std::lock_guard<std::mutex> lock(s_mutex);
						s_outbox += jsonLine(ev);
					}
					if (!lines.empty())
					{
						std::lock_guard<std::mutex> lock(s_mutex);
						for (size_t k = 0; k < lines.size(); ++k)
							s_inbox.push_back(lines[k]);
					}
				}
			}

			// write
			if (!drop)
			{
				std::lock_guard<std::mutex> lock(s_mutex);
				if (!s_outbox.empty())
				{
					int n = ::send(client, s_outbox.data(), (int)std::min<size_t>(s_outbox.size(), 1u << 20), 0);
					if (n > 0)
						s_outbox.erase(0, (size_t)n);
					else if (n == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
						drop = true;
				}
				if (s_outbox.size() > MAX_PENDING_OUT)
					drop = true;
			}

			if (drop)
			{
				closesocket(client);
				client = INVALID_SOCKET;
				std::lock_guard<std::mutex> lock(s_mutex);
				s_hasClient = false;
				s_outbox.clear();
				Inbound ev = { Inbound::DISCONNECTED, myGen, std::string() };
				s_inbox.push_back(ev);
			}
		}
	}
}

//-------------------------------------------------------------------------------------------------
bool serverStart(int port, const std::string& build)
{
	s_build = build;

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return false;

	SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET)
		return false;

	BOOL exclusive = TRUE;
	setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));

	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((u_short)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);	// protocol §1: 127.0.0.1 only, never 0.0.0.0

	if (::bind(s, (const sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR || ::listen(s, 4) == SOCKET_ERROR)
	{
		closesocket(s);
		return false;
	}

	s_listen = s;
	std::thread t(threadMain);
	t.detach();	// lives for the whole process; the OS reclaims it at exit
	return true;
}

//-------------------------------------------------------------------------------------------------
void serverPoll(std::vector<Inbound>& out)
{
	std::lock_guard<std::mutex> lock(s_mutex);
	while (!s_inbox.empty())
	{
		out.push_back(s_inbox.front());
		s_inbox.pop_front();
	}
}

//-------------------------------------------------------------------------------------------------
void serverSend(unsigned gen, const std::string& line)
{
	std::lock_guard<std::mutex> lock(s_mutex);
	if (!s_hasClient || gen != s_gen)
		return;	// stale: that client is gone
	s_outbox += line;
	s_outbox.push_back('\n');
}

//-------------------------------------------------------------------------------------------------
void serverPublishStatus(bool inGame, unsigned frame)
{
	s_inGame.store(inGame);
	s_frame.store(frame);
}

}	// namespace AIHook
