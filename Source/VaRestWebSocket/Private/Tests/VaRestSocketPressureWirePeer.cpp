// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketPressureWirePeer.h"

#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_LINUX

#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace VaRestPressureWire
{
namespace
{

void Fail(FPeer& Peer, EPeerFailure Failure)
{
	EPeerFailure Expected = EPeerFailure::None;
	if (Peer.Failure.compare_exchange_strong(Expected, Failure, std::memory_order_acq_rel)) Peer.ErrorNumber.store(errno, std::memory_order_release);
}

bool Nonblocking(int Fd)
{
	const int Flags = fcntl(Fd, F_GETFL, 0);
	return Flags >= 0 && fcntl(Fd, F_SETFL, Flags | O_NONBLOCK) == 0 && fcntl(Fd, F_SETFD, FD_CLOEXEC) == 0;
}

void DrainWake(int Fd)
{
	uint8_t Bytes[64];
	while (recv(Fd, Bytes, sizeof(Bytes), 0) > 0) {}
}

int Wait(FPeer& Peer, int Fd, short Events, double Until)
{
	for (;;)
	{
		if (Peer.bStop.load(std::memory_order_acquire)) return 0;
		const double Left = Until - NativeSeconds();
		if (Left <= 0.0) { Fail(Peer, EPeerFailure::Deadline); return -1; }
		pollfd Fds[2]{{Fd, Events, 0}, {Peer.WakeRead, POLLIN, 0}};
		const int Milliseconds = Left >= 1.0 ? 1000 : static_cast<int>(Left * 1000.0) + 1;
		const int Result = poll(Fds, 2, Milliseconds);
		if (Result < 0 && errno == EINTR) continue;
		if (Result < 0) { Fail(Peer, EPeerFailure::Poll); return -1; }
		if (Fds[1].revents & POLLIN) DrainWake(Peer.WakeRead);
		if (Peer.bStop.load(std::memory_order_acquire)) return 0;
		if (Fds[0].revents & (Events | POLLHUP | POLLERR | POLLNVAL)) return 1;
		// A controller wake is a positive reason to re-read native phase flags even without socket readiness.
		if (Fds[1].revents & POLLIN) return 1;
	}
}

bool SendAll(FPeer& Peer, int Fd, const uint8_t* Bytes, uint32_t Num, double Until)
{
	uint32_t Offset = 0;
	while (Offset < Num && !Peer.bStop.load(std::memory_order_acquire))
	{
		if (NativeSeconds() >= Until) { Fail(Peer, EPeerFailure::Deadline); return false; }
		const ssize_t Sent = send(Fd, Bytes + Offset, Num - Offset, MSG_NOSIGNAL);
		if (Sent > 0) { Offset += static_cast<uint32_t>(Sent); Peer.ResponseBytes.store(Offset, std::memory_order_release); continue; }
		if (Sent < 0 && errno == EINTR) continue;
		if (Sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { if (Wait(Peer, Fd, POLLOUT, Until) > 0) continue; return false; }
		Fail(Peer, EPeerFailure::Write); return false;
	}
	return Offset == Num;
}

bool EqualIgnoreCase(const char* A, const char* B)
{
	for (; *A && *B; ++A, ++B)
	{
		const char Left = *A >= 'A' && *A <= 'Z' ? static_cast<char>(*A + ('a' - 'A')) : *A;
		const char Right = *B >= 'A' && *B <= 'Z' ? static_cast<char>(*B + ('a' - 'A')) : *B;
		if (Left != Right) return false;
	}
	return *A == *B;
}

bool HasUpgradeToken(char* Value)
{
	for (char* Token = Value; Token;)
	{
		char* Comma = std::strchr(Token, ',');
		if (Comma) *Comma = 0;
		while (*Token == ' ' || *Token == '\t') ++Token;
		char* End = Token + std::strlen(Token);
		while (End > Token && (End[-1] == ' ' || End[-1] == '\t')) *--End = 0;
		if (EqualIgnoreCase(Token, "Upgrade")) return true;
		Token = Comma ? Comma + 1 : nullptr;
	}
	return false;
}

bool Handshake(FPeer& Peer, int Fd, double Until)
{
	uint32_t Used = 0;
	for (;;)
	{
		if (Peer.bStop.load(std::memory_order_acquire)) return false;
		if (NativeSeconds() >= Until) { Fail(Peer, EPeerFailure::Deadline); return false; }
		if (Used == sizeof(Peer.Request) - 1) { Fail(Peer, EPeerFailure::Limit); return false; }
		const ssize_t Got = recv(Fd, Peer.Request + Used, sizeof(Peer.Request) - 1 - Used, 0);
		if (Got > 0)
		{
			Used += static_cast<uint32_t>(Got); Peer.Request[Used] = 0;
			Peer.RequestBytes.store(Used, std::memory_order_release);
			if (std::strstr(Peer.Request, "\r\n\r\n")) break;
			continue;
		}
		if (Got < 0 && errno == EINTR) continue;
		if (Got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { if (Wait(Peer, Fd, POLLIN, Until) > 0) continue; return false; }
		Fail(Peer, EPeerFailure::Read); return false;
	}
	char Expected[160];
	std::snprintf(Expected, sizeof(Expected), "GET %s HTTP/1.1", Peer.Path);
	char* LineEnd = std::strstr(Peer.Request, "\r\n");
	if (!LineEnd) { Fail(Peer, EPeerFailure::Handshake); return false; }
	*LineEnd = 0;
	bool Host = false, Upgrade = false, Connection = false, Version = false, Key = false;
	if (std::strcmp(Peer.Request, Expected) != 0) { Fail(Peer, EPeerFailure::Handshake); return false; }
	for (char* Line = LineEnd + 2; *Line; Line = LineEnd + 2)
	{
		LineEnd = std::strstr(Line, "\r\n");
		if (!LineEnd) { Fail(Peer, EPeerFailure::Handshake); return false; }
		*LineEnd = 0;
		if (!*Line) break;
		char* Colon = std::strchr(Line, ':');
		if (!Colon) { Fail(Peer, EPeerFailure::Handshake); return false; }
		*Colon++ = 0; while (*Colon == ' ' || *Colon == '\t') ++Colon;
		char* End = Colon + std::strlen(Colon);
		while (End > Colon && (End[-1] == ' ' || End[-1] == '\t')) *--End = 0;
		if (EqualIgnoreCase(Line, "Host"))
		{
			if (Host || std::strcmp(Colon, Peer.Host) != 0) { Fail(Peer, EPeerFailure::Handshake); return false; }
			Host = true;
		}
		else if (EqualIgnoreCase(Line, "Upgrade"))
		{
			if (Upgrade || !EqualIgnoreCase(Colon, "websocket")) { Fail(Peer, EPeerFailure::Handshake); return false; }
			Upgrade = true;
		}
		else if (EqualIgnoreCase(Line, "Connection"))
		{
			if (Connection || !HasUpgradeToken(Colon)) { Fail(Peer, EPeerFailure::Handshake); return false; }
			Connection = true;
		}
		else if (EqualIgnoreCase(Line, "Sec-WebSocket-Version"))
		{
			if (Version || std::strcmp(Colon, "13") != 0) { Fail(Peer, EPeerFailure::Handshake); return false; }
			Version = true;
		}
		else if (EqualIgnoreCase(Line, "Sec-WebSocket-Key"))
		{
			if (Key || std::strlen(Colon) != 24) { Fail(Peer, EPeerFailure::Handshake); return false; }
			std::memcpy(Peer.Key, Colon, 24); Key = true;
		}
	}
	if (!Host || !Upgrade || !Connection || !Version || !Key) { Fail(Peer, EPeerFailure::Handshake); return false; }
	Peer.bRequestReady.store(true, std::memory_order_release);
	while (!Peer.bResponseReady.load(std::memory_order_acquire))
		if (Wait(Peer, -1, 0, Until) <= 0) return false;
	if (Peer.ResponseLength == 0 || Peer.ResponseLength > sizeof(Peer.Response)) { Fail(Peer, EPeerFailure::Handshake); return false; }
	return SendAll(Peer, Fd, reinterpret_cast<const uint8_t*>(Peer.Response), Peer.ResponseLength, Until);
}

/** Finish only the already-started server frame before echoing a Close control frame. */
bool FinishCurrentFrame(const uint8_t* Bytes, uint32_t Num, uint32_t Offset, uint32_t& End)
{
	if (Offset > Num) return false;
	uint32_t Position = 0;
	while (Position < Offset)
	{
		if (Num - Position < 2 || (Bytes[Position + 1] & 0x80)) return false;
		uint64_t Length = Bytes[Position + 1] & 127;
		uint32_t Header = 2;
		if (Length == 126)
		{
			if (Num - Position < 4) return false;
			Length = static_cast<uint32_t>(Bytes[Position + 2]) * 256 + Bytes[Position + 3]; Header = 4;
		}
		else if (Length == 127)
		{
			if (Num - Position < 10 || (Bytes[Position + 2] & 0x80)) return false;
			Length = 0; Header = 10;
			for (uint32_t Index = 2; Index < 10; ++Index) Length = (Length << 8) | Bytes[Position + Index];
		}
		if (Length > Num - Position - Header) return false;
		Position += Header + static_cast<uint32_t>(Length);
	}
	End = Position;
	return true;
}

void Exchange(FPeer& Peer, int Fd, double Until)
{
	uint8_t Input[4110]{}, Control[127]{};
	uint32_t Have = 0, ControlBytes = 0, ControlOffset = 0, TxBytes = 0, TxOffset = 0, Frames = 0;
	uint64_t Command = 0;
	bool Closing = false, FirstPong = false;
	while (!Peer.bStop.load(std::memory_order_acquire))
	{
		if (NativeSeconds() >= Until) { Fail(Peer, EPeerFailure::Deadline); return; }
		const uint64_t Requested = Peer.TxRequest.load(std::memory_order_acquire);
		if (!Closing && Command != Requested && TxOffset == TxBytes)
		{
			Command = Requested; TxBytes = Peer.TxLength; TxOffset = 0; Peer.TxSent.store(0, std::memory_order_release);
			if (TxBytes > FPeer::TxCapacity) { Fail(Peer, EPeerFailure::Limit); return; }
		}
		if (TxOffset < TxBytes)
		{
			const ssize_t Sent = send(Fd, Peer.Tx + TxOffset, TxBytes - TxOffset, MSG_NOSIGNAL);
			if (Sent > 0) { TxOffset += static_cast<uint32_t>(Sent); Peer.TxSent.store(TxOffset, std::memory_order_release); }
			else if (Sent < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) { Fail(Peer, EPeerFailure::Write); return; }
		}
		if (TxOffset == TxBytes && Command && Peer.TxCompleted.load(std::memory_order_relaxed) != Command)
			Peer.TxCompleted.store(Command, std::memory_order_release);
		if (TxOffset == TxBytes && ControlOffset < ControlBytes)
		{
			const ssize_t Sent = send(Fd, Control + ControlOffset, ControlBytes - ControlOffset, MSG_NOSIGNAL);
			if (Sent > 0) ControlOffset += static_cast<uint32_t>(Sent);
			else if (Sent < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) { Fail(Peer, EPeerFailure::Write); return; }
			if (ControlOffset == ControlBytes)
			{
				if (Closing) { Peer.bCloseEchoed.store(true, std::memory_order_release); return; }
				Peer.bFirstPongSent.store(true, std::memory_order_release);
				ControlBytes = ControlOffset = 0;
			}
		}
		const ssize_t Got = recv(Fd, Input + Have, sizeof(Input) - Have, 0);
		if (Got > 0) Have += static_cast<uint32_t>(Got);
		else if (Got == 0) { if (!Closing) Fail(Peer, EPeerFailure::Read); return; }
		else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) { Fail(Peer, EPeerFailure::Read); return; }
		while (Have >= 2)
		{
			const uint8_t Op = Input[0] & 15;
			if ((Input[0] & 0xf0) != 0x80 || !(Input[1] & 0x80) || (Op != 1 && Op != 2 && Op != 8 && Op != 9 && Op != 10)) { Fail(Peer, EPeerFailure::Frame); return; }
			uint32_t Length = Input[1] & 127, Header = 2;
			if (Length == 127) { Fail(Peer, EPeerFailure::Limit); return; }
			if (Length == 126)
			{
				if (Have < 4) break;
				Length = static_cast<uint32_t>(Input[2]) * 256 + Input[3]; Header = 4;
			}
			if (Length > 4096 || (Op >= 8 && Length > 125)) { Fail(Peer, EPeerFailure::Limit); return; }
			if (Have < Header + 4 + Length) break;
			if (++Frames > 4096) { Fail(Peer, EPeerFailure::Limit); return; }
			uint8_t* Payload = Input + Header + 4;
			for (uint32_t Index = 0; Index < Length; ++Index) Payload[Index] ^= Input[Header + Index % 4];
			if (Op == 9)
			{
				Peer.PingsSeen.fetch_add(1, std::memory_order_relaxed);
				if (!FirstPong)
				{
					FirstPong = true; Control[0] = 0x8a; Control[1] = static_cast<uint8_t>(Length);
					std::memcpy(Control + 2, Payload, Length); ControlBytes = Length + 2; ControlOffset = 0;
				}
			}
			else if (Op == 8)
			{
				if (Length < 2 || ControlBytes != ControlOffset) { Fail(Peer, EPeerFailure::Frame); return; }
				Peer.CloseCode.store(static_cast<uint32_t>(Payload[0]) * 256 + Payload[1], std::memory_order_relaxed);
				Peer.bCloseSeen.store(true, std::memory_order_release);
				Closing = true; Control[0] = 0x88; Control[1] = static_cast<uint8_t>(Length);
				std::memcpy(Control + 2, Payload, Length); ControlBytes = Length + 2; ControlOffset = 0;
				if (TxOffset < TxBytes)
				{
					uint32_t FrameEnd = 0;
					if (!FinishCurrentFrame(Peer.Tx, TxBytes, TxOffset, FrameEnd)) { Fail(Peer, EPeerFailure::Frame); return; }
					TxBytes = FrameEnd;
				}
			}
			else if (Op == 1 || Op == 2) Peer.ClientMessages.fetch_add(1, std::memory_order_release);
			const uint32_t Used = Header + 4 + Length;
			Have -= Used; std::memmove(Input, Input + Used, Have);
		}
		if (Have == sizeof(Input)) { Fail(Peer, EPeerFailure::Limit); return; }
		const bool Writable = TxOffset < TxBytes || ControlOffset < ControlBytes;
		if (Wait(Peer, Fd, static_cast<short>(POLLIN | (Writable ? POLLOUT : 0)), Until) <= 0) return;
	}
}

void* Run(void* Argument)
{
	FPeer& Peer = *static_cast<FPeer*>(Argument);
	int Accepted = -1;
	const double HandshakeUntil = NativeSeconds() + 5.0;
	while (!Peer.bStop.load(std::memory_order_acquire))
	{
		if (NativeSeconds() >= HandshakeUntil) { Fail(Peer, EPeerFailure::Deadline); break; }
		sockaddr_in Address{}; socklen_t AddressBytes = sizeof(Address);
		Accepted = accept(Peer.Listener, reinterpret_cast<sockaddr*>(&Address), &AddressBytes);
		if (Accepted >= 0)
		{
			Peer.AcceptedTcp.store(1, std::memory_order_release);
			if (Address.sin_family != AF_INET || ntohl(Address.sin_addr.s_addr) != INADDR_LOOPBACK || !Nonblocking(Accepted)) Fail(Peer, EPeerFailure::Accept);
			break;
		}
		if (errno == EINTR) continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK) { if (Wait(Peer, Peer.Listener, POLLIN, HandshakeUntil) > 0) continue; break; }
		Fail(Peer, EPeerFailure::Accept); break;
	}
	if (Accepted >= 0 && Peer.Failure.load(std::memory_order_acquire) == EPeerFailure::None && Handshake(Peer, Accepted, HandshakeUntil))
		Exchange(Peer, Accepted, NativeSeconds() + 120.0);
	if (Accepted >= 0) close(Accepted);
	if (Peer.Listener >= 0) close(Peer.Listener);
	if (Peer.WakeRead >= 0) close(Peer.WakeRead);
	Peer.bDone.store(true, std::memory_order_release);
	return nullptr;
}

}

double NativeSeconds()
{
	timespec Time{};
	clock_gettime(CLOCK_MONOTONIC, &Time);
	return static_cast<double>(Time.tv_sec) + static_cast<double>(Time.tv_nsec) / 1000000000.0;
}

bool StartPeer(FPeer& Peer, const char* Path)
{
	if (std::strlen(Path) >= sizeof(Peer.Path)) return false;
	std::strcpy(Peer.Path, Path);
	int Wake[2]{-1, -1};
	Peer.Listener = socket(AF_INET, SOCK_STREAM, 0);
	if (Peer.Listener < 0 || !Nonblocking(Peer.Listener) || socketpair(AF_UNIX, SOCK_STREAM, 0, Wake) != 0) { Fail(Peer, EPeerFailure::Setup); }
	else
	{
		Peer.WakeRead = Wake[0]; Peer.WakeWrite = Wake[1];
		sockaddr_in Address{}; Address.sin_family = AF_INET; Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		socklen_t AddressBytes = sizeof(Address);
		if (!Nonblocking(Peer.WakeRead) || !Nonblocking(Peer.WakeWrite)
			|| bind(Peer.Listener, reinterpret_cast<sockaddr*>(&Address), sizeof(Address)) != 0
			|| getsockname(Peer.Listener, reinterpret_cast<sockaddr*>(&Address), &AddressBytes) != 0 || listen(Peer.Listener, 1) != 0)
			Fail(Peer, EPeerFailure::Setup);
		else
		{
			Peer.Port = ntohs(Address.sin_port);
			std::snprintf(Peer.Host, sizeof(Peer.Host), "127.0.0.1:%u", static_cast<unsigned>(Peer.Port));
			const int Created = pthread_create(&Peer.Thread, nullptr, &Run, &Peer);
			if (Created == 0) { Peer.bStarted = true; return true; }
			Fail(Peer, EPeerFailure::Thread); Peer.ErrorNumber.store(Created, std::memory_order_release);
		}
	}
	// No worker exists on setup failure, so these descriptors still belong to this caller.
	if (Peer.Listener >= 0) { close(Peer.Listener); Peer.Listener = -1; }
	if (Peer.WakeRead >= 0) { close(Peer.WakeRead); Peer.WakeRead = -1; }
	if (Peer.WakeWrite >= 0) { close(Peer.WakeWrite); Peer.WakeWrite = -1; }
	Peer.bDone.store(true, std::memory_order_release);
	return false;
}

void WakePeer(FPeer& Peer)
{
	if (Peer.WakeWrite < 0 || Peer.bDone.load(std::memory_order_acquire)) return;
	const uint8_t Byte = 1;
	const double Until = NativeSeconds() + 0.1;
	for (;;)
	{
		const ssize_t Wrote = send(Peer.WakeWrite, &Byte, 1, MSG_NOSIGNAL);
		// EAGAIN already leaves a readable wake byte; other non-EINTR results also cover intentional peer exit.
		if (Wrote >= 0 || errno != EINTR || Peer.bDone.load(std::memory_order_acquire)) return;
		if (NativeSeconds() >= Until) { Fail(Peer, EPeerFailure::Deadline); return; }
	}
}

void CancelPeer(FPeer& Peer)
{
	Peer.bStop.store(true, std::memory_order_release);
	if (!Peer.bStarted) Peer.bDone.store(true, std::memory_order_release);
	WakePeer(Peer);
}

void JoinPeer(FPeer& Peer)
{
	if (Peer.bStarted && !Peer.bJoined)
	{
		CancelPeer(Peer);
		const int Result = pthread_join(Peer.Thread, nullptr);
		if (Result == 0) Peer.bJoined = true;
		else { Fail(Peer, EPeerFailure::Thread); Peer.ErrorNumber.store(Result, std::memory_order_release); }
	}
	if (Peer.WakeWrite >= 0) { close(Peer.WakeWrite); Peer.WakeWrite = -1; }
}

}

#endif
