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
	const size_t MAX_PENDING_OUT = 64u * 1024u * 1024u;	///< drop a client that stops reading
	const DWORD EOF_DRAIN_MS = 5000;										///< after a client half-close, wait this long for replies

	std::mutex s_mutex;													///< guards everything below
	std::deque<Inbound> s_inbox;
	std::string s_outbox;												///< bytes waiting to be written to the client
	size_t s_outOff = 0;												///< bytes of s_outbox already sent
	unsigned s_gen = 0;													///< current connection generation
	bool s_hasClient = false;
	unsigned s_linesInFlight = 0;								///< lines handed to the main thread, not yet answered

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

	/// Tell a second client BUSY, then close gracefully so the line isn't lost to a reset:
	/// half-close our side and briefly drain whatever it sent.
	void rejectBusy(SOCKET s)
	{
		const std::string line = busyLine();
		size_t off = 0;
		while (off < line.size())
		{
			int n = ::send(s, line.data() + off, (int)(line.size() - off), 0);
			if (n <= 0)
				break;
			off += (size_t)n;
		}
		shutdown(s, SD_SEND);
		setNonBlocking(s);
		char junk[4096];
		for (int tries = 0; tries < 20; ++tries)
		{
			fd_set r;
			FD_ZERO(&r);
			FD_SET(s, &r);
			timeval tv;
			tv.tv_sec = 0;
			tv.tv_usec = 10000;
			if (::select(0, &r, nullptr, nullptr, &tv) <= 0)
				continue;
			int n = ::recv(s, junk, sizeof(junk), 0);
			if (n <= 0)
				break;
		}
		closesocket(s);
	}

	SOCKET openListener(int port)
	{
		SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s == INVALID_SOCKET)
			return INVALID_SOCKET;
		// no SO_EXCLUSIVEADDRUSE: it makes bind fail while an old connection on the port is in TIME_WAIT
		// (e.g. right after the game restarts); without SO_REUSEADDR a live listener still can't be shared.
		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons((u_short)port);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);	// protocol §1: 127.0.0.1 only, never 0.0.0.0
		if (::bind(s, (const sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR || ::listen(s, 4) == SOCKET_ERROR)
		{
			closesocket(s);
			return INVALID_SOCKET;
		}
		return s;
	}

	void pushOverlongError()
	{
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

	void threadMain(int port)
	{
		SOCKET client = INVALID_SOCKET;
		std::string inbuf;
		bool discardingOverlong = false;
		bool peerEof = false;
		DWORD eofTime = 0;
		unsigned myGen = 0;
		std::vector<char> rbuf(64 * 1024);

		for (;;)
		{
			// bind (retried: the port may be briefly busy after a restart)
			if (s_listen == INVALID_SOCKET)
			{
				s_listen = openListener(port);
				if (s_listen == INVALID_SOCKET)
				{
					Sleep(2000);
					continue;
				}
			}

			fd_set rset, wset;
			FD_ZERO(&rset);
			FD_ZERO(&wset);
			FD_SET(s_listen, &rset);
			if (client != INVALID_SOCKET)
			{
				if (!peerEof)
					FD_SET(client, &rset);
				std::lock_guard<std::mutex> lock(s_mutex);
				if (s_outOff < s_outbox.size())
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
						rejectBusy(s);
					else
					{
						setNonBlocking(s);
						BOOL nodelay = TRUE;
						setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
						client = s;
						inbuf.clear();
						discardingOverlong = false;
						peerEof = false;
						std::lock_guard<std::mutex> lock(s_mutex);
						myGen = ++s_gen;
						s_hasClient = true;
						s_linesInFlight = 0;
						s_outbox = helloLine();		// hello is always the first line
						s_outOff = 0;
						Inbound ev = { Inbound::CONNECTED, myGen, std::string() };
						s_inbox.push_back(ev);
					}
				}
			}

			if (client == INVALID_SOCKET)
				continue;

			bool drop = false;

			// read
			if (!peerEof && FD_ISSET(client, &rset))
			{
				int n = ::recv(client, rbuf.data(), (int)rbuf.size(), 0);
				if (n == 0)
				{
					// half-close: stop reading but still answer what was already sent (protocol §2)
					peerEof = true;
					eofTime = GetTickCount();
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
					for (int i = 0; i <= n; ++i)
					{
						const bool atEnd = (i == n);
						if (!atEnd && rbuf[i] != '\n')
							continue;
						const size_t fragLen = (size_t)i - start;
						if (!discardingOverlong)
						{
							// protocol §1: max line length 4 MiB, checked for every fragment
							if (inbuf.size() + fragLen > (size_t)MAX_LINE_BYTES)
							{
								inbuf.clear();
								discardingOverlong = true;
								pushOverlongError();
							}
							else
								inbuf.append(rbuf.data() + start, fragLen);
						}
						if (atEnd)
							break;
						if (!discardingOverlong)
						{
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
					if (!lines.empty())
					{
						std::lock_guard<std::mutex> lock(s_mutex);
						for (size_t k = 0; k < lines.size(); ++k)
							s_inbox.push_back(lines[k]);
						s_linesInFlight += (unsigned)lines.size();
					}
				}
			}

			// write (send outside the lock from a private copy of the next chunk)
			if (!drop)
			{
				std::string chunk;
				{
					std::lock_guard<std::mutex> lock(s_mutex);
					if (s_outOff < s_outbox.size())
						chunk.assign(s_outbox, s_outOff, std::min<size_t>(s_outbox.size() - s_outOff, 1u << 20));
					if (s_outbox.size() - s_outOff > MAX_PENDING_OUT)
						drop = true;
				}
				if (!drop && !chunk.empty())
				{
					int n = ::send(client, chunk.data(), (int)chunk.size(), 0);
					std::lock_guard<std::mutex> lock(s_mutex);
					if (n > 0)
					{
						s_outOff += (size_t)n;
						if (s_outOff == s_outbox.size())
						{
							s_outbox.clear();
							s_outOff = 0;
						}
						else if (s_outOff > (4u << 20))
						{
							s_outbox.erase(0, s_outOff);
							s_outOff = 0;
						}
					}
					else if (n == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
						drop = true;
				}
			}

			// after a half-close, close once every queued request is answered and flushed
			if (!drop && peerEof)
			{
				std::lock_guard<std::mutex> lock(s_mutex);
				bool queued = false;
				for (size_t k = 0; k < s_inbox.size(); ++k)
					if (s_inbox[k].kind == Inbound::LINE && s_inbox[k].gen == myGen)
						queued = true;
				const bool flushed = s_outOff >= s_outbox.size();
				if ((!queued && s_linesInFlight == 0 && flushed) || GetTickCount() - eofTime > EOF_DRAIN_MS)
					drop = true;
			}

			if (drop)
			{
				closesocket(client);
				client = INVALID_SOCKET;
				peerEof = false;
				std::lock_guard<std::mutex> lock(s_mutex);
				s_hasClient = false;
				s_outbox.clear();
				s_outOff = 0;
				s_linesInFlight = 0;
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

	s_listen = openListener(port);	// first try now; the thread keeps retrying if the port is busy
	std::thread t(threadMain, port);
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
void serverLinesAnswered(unsigned gen, unsigned count)
{
	std::lock_guard<std::mutex> lock(s_mutex);
	if (gen != s_gen)
		return;
	s_linesInFlight = count >= s_linesInFlight ? 0 : s_linesInFlight - count;
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
