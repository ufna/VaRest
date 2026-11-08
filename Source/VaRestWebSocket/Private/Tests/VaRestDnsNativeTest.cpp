// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "VaRestDnsNative.h"
#include "VaRestLwsTransport.h"

#if VAREST_HAS_PRIVATE_DNS
#include <errno.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace VaRest::WebSocket::Lws::VaRestDnsNativeTestPrivate
{
	using namespace VaRest::WebSocket::Dns;
	bool Signal(int Fd, uint64 Count)
	{
		ssize_t N;
		do { N = write(Fd, &Count, sizeof(Count)); } while (N < 0 && errno == EINTR);
		return N == sizeof(Count);
	}
	template<typename F> bool Until(F&& Predicate, double Seconds = 5.0)
	{
		const double End = FPlatformTime::Seconds() + Seconds;
		do
		{
			(void)PollRetirement();
			if (Predicate()) return true;
			FPlatformProcess::SleepNoStats(0.001f);
		} while (FPlatformTime::Seconds() < End);
		return false;
	}

	/** All borrowed gate descriptors stay open until the actual child join, even on an assertion/error path. */
	struct FGateOwner
	{
		int Before = eventfd(0, EFD_CLOEXEC | EFD_SEMAPHORE);
		int After = eventfd(0, EFD_CLOEXEC | EFD_SEMAPHORE);
		TArray<FTicket> Tickets;
		bool bRetired = false;
		bool ReleaseAndRetire()
		{
			for (const FTicket& Ticket : Tickets) Discard(Ticket);
			if (Before >= 0) Signal(Before, 64);
			if (After >= 0) Signal(After, 64);
			bRetired = Until([] { FStats S{}; return SnapshotForTests(S) == EStatus::Ready && S.Occupied == 0; });
			return bRetired;
		}
		~FGateOwner()
		{
			if (!bRetired) ReleaseAndRetire();
			// Do not close/reuse an fd still borrowed by an uncertain native child. Failure stays a failed test.
			if (bRetired) { if (Before >= 0) close(Before); if (After >= 0) close(After); }
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestDnsNativeRetirementTest, "VaRest.WebSocket.Lws.PrivateDnsRetirement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestDnsNativeRetirementTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Dns;
	using namespace VaRest::WebSocket::Lws;
	using namespace VaRest::WebSocket::Lws::VaRestDnsNativeTestPrivate;
	if (!TestTrue(TEXT("actual GT threading phase initializes native storage"), InitializeOnGT(PrivateDnsThreadsAllowedOnGT()) == EStatus::Ready)) return false;
	FStats Initial{};
	if (!TestTrue(TEXT("the isolated fixture starts with no outstanding native interests"), SnapshotForTests(Initial) == EStatus::Ready && Initial.Occupied == 0 && !Initial.bClosed)) return false;
	FGateOwner Gates;
	if (!TestTrue(TEXT("both real native eventfd gates were created"), Gates.Before >= 0 && Gates.After >= 0)) return false;
	FTicket First{};
	if (!TestTrue(TEXT("a real libc localhost resolution is queued, with owned input"), Submit("localhost", 9, 80, First, Gates.Before, Gates.After) == EStatus::Queued)) return false;
	Gates.Tickets.Add(First);
	if (!TestTrue(TEXT("the actual child is held before its resolver call"), Until([] { FStats S{}; return SnapshotForTests(S) == EStatus::Ready && S.BeforeResolverWaiting == 1; }))) return false;
	FStats Held{}; SnapshotForTests(Held);
	TestEqual(TEXT("no getaddrinfo call happened before the native gate"), Held.ResolverCalls, Initial.ResolverCalls);
	Discard(First);
	FAddress Address{}; FError Error{};
	TestTrue(TEXT("logical discard is visible before physical retirement"), Next(First, Address, Error) == EStatus::Stopped);
	for (uint32 I = 1; I < Initial.Capacity; ++I)
	{
		FTicket Ticket{};
		if (!TestTrue(TEXT("every still-running generation occupies one real native slot"), Submit("localhost", 9, 80, Ticket, Gates.Before) == EStatus::Queued)) return false;
		Gates.Tickets.Add(Ticket);
	}
	FTicket Refused{};
	TestTrue(TEXT("canceled but unjoined work cannot be reused to exceed actual capacity"), Submit("localhost", 9, 80, Refused, Gates.Before) == EStatus::Capacity && Refused.Generation == 0);
	for (const FTicket& Ticket : Gates.Tickets) Discard(Ticket);
	if (!TestTrue(TEXT("release all real resolver calls"), Signal(Gates.Before, Initial.Capacity))) return false;
	if (!TestTrue(TEXT("publication alone does not join/reuse the first worker"), Until([] {
		FStats S{}; return SnapshotForTests(S) == EStatus::Ready && S.Occupied == 1 && S.Unjoined == 1 && S.PublishedUnjoined == 1 && S.AfterPublishedWaiting == 1;
	}))) return false;
	FStats Published{}; SnapshotForTests(Published);
	TestEqual(TEXT("the canceled first job still made its genuine libc call after the gate"), Published.ResolverCalls, Initial.ResolverCalls + Initial.Capacity);
	TestEqual(TEXT("only actual completed joins freed the other generations"), Published.Joined, Initial.Joined + Initial.Capacity - 1);
	FTicket Replacement{};
	if (!TestTrue(TEXT("a joined slot can own a new generation"), Submit("localhost", 9, 80, Replacement, Gates.Before) == EStatus::Queued)) return false;
	Gates.Tickets.Add(Replacement);
	const FTicket Stale = Gates.Tickets[1];
	TestTrue(TEXT("the replacement reuses only the joined second slot with a new generation"), Stale.Slot == Replacement.Slot && Stale.Generation != Replacement.Generation);
	Discard(Stale); // Must not revoke Replacement's interest.
	if (!TestTrue(TEXT("release the replacement's actual resolver"), Signal(Gates.Before, 1))) return false;
	EStatus Result = EStatus::Pending;
	if (!TestTrue(TEXT("a stale discard cannot steal the new joined address list"), Until([&] {
		Result = Next(Replacement, Address, Error);
		return Result != EStatus::Pending && Result != EStatus::Busy;
	}) && Result == EStatus::Ready && (Address.Family == 4 || Address.Family == 6))) return false;
	FStats StillPublished{}; SnapshotForTests(StillPublished);
	TestTrue(TEXT("the original published child is still unjoined while a different generation succeeds"), StillPublished.Unjoined == 1 && StillPublished.AfterPublishedWaiting == 1);
	TestTrue(TEXT("all gates released and every actual native child joined before descriptor reuse"), Gates.ReleaseAndRetire());
	FStats Final{}; SnapshotForTests(Final);
	TestEqual(TEXT("actual created handles all have successful joins"), Final.Created - Initial.Created, Final.Joined - Initial.Joined);
	TestEqual(TEXT("no poisoned generation is hidden by cancellation"), Final.Poisoned, Initial.Poisoned);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestDnsConnectLifetimeTest, "VaRest.WebSocket.Lws.PrivateDnsConnectLifetime",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestDnsConnectLifetimeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Dns;
	using namespace VaRest::WebSocket::Lws;
	using namespace VaRest::WebSocket::Lws::VaRestDnsNativeTestPrivate;
	if (!TestTrue(TEXT("actual GT threading phase is ready"), InitializeOnGT(PrivateDnsThreadsAllowedOnGT()) == EStatus::Ready)) return false;
	for (bool bCancel : { false, true })
	{
		FStats Initial{};
		if (!TestTrue(TEXT("the previous fixture has no outstanding DNS worker"), SnapshotForTests(Initial) == EStatus::Ready && Initial.Occupied == 0)) return false;
		FGateOwner Gate;
		if (!TestTrue(TEXT("native before-resolver gate exists"), Gate.Before >= 0 && Gate.After >= 0)) return false;
		FServiceOptions Options;
		Options.DnsBeforeResolverGateForTest = Gate.Before;
		Options.ActiveWaitCapMs = Options.ShortWaitCapMs = 5;
		Options.ExitFlushSeconds = 0;
		Options.bOverrideAllowlist = true;
		Options.Allowlist.Emplace(TEXT("ws"), TArray<FString>{TEXT("localhost")});
		FTransport Transport(MoveTemp(Options));
		FVaRestSocketConnectRequest Request;
		Request.Url = TEXT("ws://localhost:9/dns-lifetime");
		Request.ConnectTimeout = bCancel ? 2.0 : 0.15;
		Request.LogName = bCancel ? TEXT("private-dns-canceled") : TEXT("private-dns-deadline");
		const auto Connection = Transport.Connect(MoveTemp(Request));
		if (!TestTrue(TEXT("a genuine FConn/FService submitted the actual held native job"), Until([] {
			FStats S{}; return SnapshotForTests(S) == EStatus::Ready && S.BeforeResolverWaiting == 1;
		}))) { Transport.Shutdown(); return false; }
		if (bCancel) Connection->Close(1000, TEXT("cancel while native DNS is held"));
		int32 Terminals = 0, Opens = 0;
		EVaRestSocketFailure Failure = EVaRestSocketFailure::None;
		if (!TestTrue(TEXT("the real service reports one terminal without releasing DNS"), Until([&] {
			TArray<FVaRestSocketEvent> Events; Connection->PopEvents(Events);
			for (const auto& Event : Events) {
				Opens += Event.Type == EVaRestSocketEventType::Open;
				if (Event.IsTerminal()) { ++Terminals; Failure = Event.End.Failure; }
			}
			return Terminals > 0;
		}, 2.0))) { Transport.Shutdown(); return false; }
		TestTrue(TEXT("deadline/cancel classification remains exact before any TCP connect"), Opens == 0 && Terminals == 1
			&& Failure == (bCancel ? EVaRestSocketFailure::Canceled : EVaRestSocketFailure::ConnectTimeout));
		TestTrue(TEXT("a no-WSI terminal is removed from the actual service"), Until([&] { const FServiceCounters* C = Transport.GetCounters(); return C && C->LiveConnections.load() == 0; }));
		FStats Held{}; SnapshotForTests(Held);
		TestTrue(TEXT("logical completion did not fake a native join"), Held.Occupied == 1 && Held.Unjoined == 1 && Held.BeforeResolverWaiting == 1);
		const double Start = FPlatformTime::Seconds();
		Transport.Shutdown();
		TestTrue(TEXT("service shutdown returned while the real resolver child is still held"), FPlatformTime::Seconds() - Start < 1.0);
		SnapshotForTests(Held);
		TestEqual(TEXT("shutdown did not claim the held child had joined"), Held.Joined, Initial.Joined);
		TestTrue(TEXT("release held work and physically retire its native storage"), Gate.ReleaseAndRetire());
		TArray<FVaRestSocketEvent> Late; Connection->PopEvents(Late);
		TestTrue(TEXT("late native completion publishes no new socket event"), Late.IsEmpty());
	}
	return true;
}

#endif // VAREST_HAS_PRIVATE_DNS
#endif // WITH_DEV_AUTOMATION_TESTS
