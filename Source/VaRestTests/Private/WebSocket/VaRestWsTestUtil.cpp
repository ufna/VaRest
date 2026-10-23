// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestWsTestUtil.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "VaRestClient.h"
#include "VaRestHttpTesting.h"
#include "VaRestSha256.h"

namespace VaRest::WsTest::VaRestWsTestUtilPrivate
{
	/** "http://127.0.0.1:41234" -> host and port (VaRestLwsProbe.cpp:25-45) */
	bool SplitUrl(const FString& Url, const TCHAR* Scheme, FString& OutHost, int32& OutPort)
	{
		const FString Prefix = FString(Scheme) + TEXT("://");
		if (!Url.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			return false;
		}
		FString Authority = Url.Mid(Prefix.Len());
		int32 Slash = INDEX_NONE;
		if (Authority.FindChar(TEXT('/'), Slash))
		{
			Authority.LeftInline(Slash);
		}
		FString PortText;
		if (!Authority.Split(TEXT(":"), &OutHost, &PortText, ESearchCase::CaseSensitive, ESearchDir::FromEnd) || !PortText.IsNumeric())
		{
			return false;
		}
		OutPort = FCString::Atoi(*PortText);
		return !OutHost.IsEmpty() && OutPort > 0 && OutPort < 65536;
	}

	/** The test CA, fetched once per run */
	TArray<uint8>& CachedCa()
	{
		static TArray<uint8> Ca;
		return Ca;
	}

	bool IsOpenOrEnd(const FVaRestSocketEvent& Event)
	{
		return Event.Type == EVaRestSocketEventType::Open || Event.IsTerminal();
	}
}

bool VaRest::WsTest::FindSite(FAutomationTestBase& Test, FSite& Out, bool bNeedTls)
{
	using namespace VaRest::WsTest::VaRestWsTestUtilPrivate;
	Out.HttpBase = VaRest::Http::Testing::GetTestServerUrl();
	if (Out.HttpBase.IsEmpty() || !SplitUrl(Out.HttpBase, TEXT("http"), Out.Host, Out.Port))
	{
		Test.AddError(TEXT("no test server: run Tools/run_tests.sh, which starts Tools/testserver/server.py and passes -VaRestTestServer=<url> and -VaRestTestServerTls=<url>"));
		return false;
	}
	FString TlsHost;
	if (!SplitUrl(VaRest::Http::Testing::GetTestServerTlsUrl(), TEXT("https"), TlsHost, Out.TlsPort))
	{
		Out.TlsPort = 0;
	}
	if (bNeedTls && Out.TlsPort == 0)
	{
		Test.AddError(TEXT("no TLS listener: -VaRestTestServerTls=<url> is missing (Tools/run_tests.sh passes it; Tools/testserver/server.py --tls-port)"));
		return false;
	}
	return true;
}

TArray<VaRest::WsTest::FScheme> VaRest::WsTest::Schemes(const FSite& Site)
{
	TArray<FScheme> Out;
	Out.Add({ TEXT("ws"), FString::Printf(TEXT("ws://%s:%d"), *Site.Host, Site.Port), false });
	// Plan decision 19: the DNS-name path through LWS's own name check; MakeTransport sends the TCP to 127.0.0.1
	Out.Add({ TEXT("wss"), FString::Printf(TEXT("wss://localhost:%d"), Site.TlsPort), true });
	return Out;
}

FString VaRest::WsTest::NewKey()
{
	return FGuid::NewGuid().ToString(EGuidFormats::Digits);
}

TSharedPtr<VaRest::WebSocket::Testing::ILwsTestTransport, ESPMode::ThreadSafe> VaRest::WsTest::MakeTransport(FAutomationTestBase& Test, const FSite& Site, bool bTrustTestCa)
{
	using namespace VaRest::WsTest::VaRestWsTestUtilPrivate;
	VaRest::WebSocket::Testing::FLwsTransportOptions Options;
	Options.TcpAddressOverride = Site.Host;
	if (bTrustTestCa)
	{
		TArray<uint8>& Ca = CachedCa();
		if (Ca.Num() == 0 && !FetchBytes(Test, Site, TEXT("/tls/ca.pem"), Ca))
		{
			return nullptr;
		}
		Options.ExtraTrustedPem = Ca;
	}
	return VaRest::WebSocket::Testing::MakeLwsTransport(MoveTemp(Options));
}

void VaRest::WsTest::ExpectLwsLines(FAutomationTestBase& Test)
{
	Test.AddExpectedMessagePlain(TEXT("libwebsockets("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
}

VaRest::WsTest::FEvents::FEvents(TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> InConnection)
	: Connection(MoveTemp(InConnection))
{
}

void VaRest::WsTest::FEvents::Pull()
{
	TArray<FVaRestSocketEvent> Got;
	Connection->PopEvents(Got);
	for (FVaRestSocketEvent& Event : Got)
	{
		if (bAcknowledge && Event.Type == EVaRestSocketEventType::Message)
		{
			Connection->Consumed(Event.Bytes.Num());
		}
		Seen.Add(MoveTemp(Event));
	}
}

bool VaRest::WsTest::FEvents::WaitFor(TFunctionRef<bool(const TArray<FVaRestSocketEvent>&)> Done, double Seconds)
{
	return VaRest::Http::Testing::PumpUntil([this, &Done] { Pull(); return Done(Seen); }, Seconds, true);
}

bool VaRest::WsTest::FEvents::WaitType(EVaRestSocketEventType Type, double Seconds)
{
	return WaitFor([Type](const TArray<FVaRestSocketEvent>& All)
	{
		return All.ContainsByPredicate([Type](const FVaRestSocketEvent& Each) { return Each.Type == Type; });
	}, Seconds);
}

bool VaRest::WsTest::FEvents::WaitEnd(double Seconds)
{
	return WaitFor([](const TArray<FVaRestSocketEvent>& All)
	{
		return All.ContainsByPredicate([](const FVaRestSocketEvent& Each) { return Each.IsTerminal(); });
	}, Seconds);
}

bool VaRest::WsTest::FEvents::WaitMessages(int32 Wanted, double Seconds)
{
	WaitFor([Wanted](const TArray<FVaRestSocketEvent>& All)
	{
		int32 Messages = 0;
		for (const FVaRestSocketEvent& Each : All)
		{
			if (Each.IsTerminal())
			{
				return true;
			}
			Messages += Each.Type == EVaRestSocketEventType::Message ? 1 : 0;
		}
		return Messages >= Wanted;
	}, Seconds);
	return Count(EVaRestSocketEventType::Message) >= Wanted;
}

const FVaRestSocketEvent* VaRest::WsTest::FEvents::Find(EVaRestSocketEventType Type) const
{
	return Seen.FindByPredicate([Type](const FVaRestSocketEvent& Each) { return Each.Type == Type; });
}

const FVaRestSocketEvent* VaRest::WsTest::FEvents::GetEnd() const
{
	return Seen.FindByPredicate([](const FVaRestSocketEvent& Each) { return Each.IsTerminal(); });
}

TArray<const FVaRestSocketEvent*> VaRest::WsTest::FEvents::GetMessages() const
{
	TArray<const FVaRestSocketEvent*> Out;
	for (const FVaRestSocketEvent& Each : Seen)
	{
		if (Each.Type == EVaRestSocketEventType::Message)
		{
			Out.Add(&Each);
		}
	}
	return Out;
}

int32 VaRest::WsTest::FEvents::Count(EVaRestSocketEventType Type) const
{
	int32 Out = 0;
	for (const FVaRestSocketEvent& Each : Seen)
	{
		Out += Each.Type == Type ? 1 : 0;
	}
	return Out;
}

FString VaRest::WsTest::FEvents::Describe() const
{
	TArray<FString> Parts;
	int32 Messages = 0;
	for (const FVaRestSocketEvent& Each : Seen)
	{
		if (Each.Type == EVaRestSocketEventType::Message)
		{
			++Messages;
			continue;
		}
		if (Messages > 0)
		{
			Parts.Add(FString::Printf(TEXT("Message x%d"), Messages));
			Messages = 0;
		}
		if (Each.IsTerminal())
		{
			Parts.Add(FString::Printf(TEXT("%s %d by %s, %s%s (%s)"), LexToString(Each.Type), Each.End.Code, LexToString(Each.End.By), LexToString(Each.End.Failure),
				Each.End.bWasClean ? TEXT(", clean") : TEXT(""), *Each.End.Detail));
		}
		else
		{
			Parts.Add(LexToString(Each.Type));
		}
	}
	if (Messages > 0)
	{
		Parts.Add(FString::Printf(TEXT("Message x%d"), Messages));
	}
	return Parts.Num() > 0 ? FString::Join(Parts, TEXT(" > ")) : FString(TEXT("(no events)"));
}

TUniquePtr<VaRest::WsTest::FEvents> VaRest::WsTest::Visit(IVaRestSocketTransport& Transport, FVaRestSocketConnectRequest&& Req, double Seconds)
{
	using namespace VaRest::WsTest::VaRestWsTestUtilPrivate;
	TUniquePtr<FEvents> Events = MakeUnique<FEvents>(Transport.Connect(MoveTemp(Req)));
	Events->WaitFor([](const TArray<FVaRestSocketEvent>& All) { return All.ContainsByPredicate(&IsOpenOrEnd); }, Seconds);
	if (Events->Find(EVaRestSocketEventType::Open) && !Events->GetEnd())
	{
		Events->Connection->Close(1000, TEXT("done"));
	}
	Events->WaitEnd(Seconds);
	return Events;
}

bool VaRest::WsTest::IsError(const FEvents& Events, EVaRestSocketFailure Failure)
{
	const FVaRestSocketEvent* End = Events.GetEnd();
	return End && End->Type == EVaRestSocketEventType::Error && End->End.Failure == Failure;
}

FVaRestSocketConnectRequest VaRest::WsTest::Request(const FScheme& Scheme, const FString& PathAndQuery)
{
	FVaRestSocketConnectRequest Out;
	Out.Url = Scheme.Base + PathAndQuery;
	return Out;
}

FVaRestJson VaRest::WsTest::FetchJournal(FAutomationTestBase& Test, const FSite& Site, const FString& Key, int32 Connections, int32 WaitMs)
{
	using namespace VaRest::Http::Testing;
	FVaRestClient Client = FVaRestClient::Make(Site.HttpBase);
	Client.WithLogLevel(EVaRestLogLevel::None);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Client.Get(FString::Printf(TEXT("/ws/journal?key=%s&connections=%d&wait_ms=%d"), *Key, Connections, WaitMs)), Outcome).Send();
	const double Seconds = WaitMs / 1000.0 + 15.0;
	if (!PumpUntil([Outcome] { return Outcome->IsDone(); }, Seconds, true))
	{
		// Drained before returning (VaRestLwsProbe.cpp:59-64): Canceled completes on a later game-thread task
		Handle.Cancel();
		PumpUntil([Outcome, &Handle] { return Outcome->IsDone() && !Handle.IsActive(); }, 5.0, true);
		Test.AddError(FString::Printf(TEXT("the journal of %s did not arrive within %.0f s"), *Key, Seconds));
		return FVaRestJson();
	}
	if (Outcome->SuccessCalls.load() != 1 || !Outcome->Value.IsSet())
	{
		Test.AddError(FString::Printf(TEXT("the journal of %s did not arrive: %s"), *Key, *Outcome->Error.Describe()));
		return FVaRestJson();
	}
	return Outcome->Value.GetValue();
}

bool VaRest::WsTest::FetchBytes(FAutomationTestBase& Test, const FSite& Site, const FString& Path, TArray<uint8>& Out)
{
	using namespace VaRest::Http::Testing;
	FVaRestClient Client = FVaRestClient::Make(Site.HttpBase);
	Client.WithLogLevel(EVaRestLogLevel::None);
	auto Outcome = MakeOutcome<TArray<uint8>>();
	const FVaRestRequestHandle Handle = Observe(Client.Get<TArray<uint8>>(Path), Outcome).Send();
	if (!PumpUntil([Outcome] { return Outcome->IsDone(); }, 20.0, true))
	{
		Handle.Cancel();
		PumpUntil([Outcome, &Handle] { return Outcome->IsDone() && !Handle.IsActive(); }, 5.0, true);
		Test.AddError(FString::Printf(TEXT("GET %s: no answer within 20 s"), *Path));
		return false;
	}
	if (Outcome->SuccessCalls.load() != 1 || !Outcome->Value.IsSet())
	{
		Test.AddError(FString::Printf(TEXT("GET %s failed: %s"), *Path, *Outcome->Error.Describe()));
		return false;
	}
	Out = Outcome->Value.GetValue();
	return true;
}

TArray<FVaRestJson> VaRest::WsTest::JournalEvents(const FVaRestJson& Journal, const TCHAR* Kind, int32 ConnIndex)
{
	TArray<FVaRestJson> Out;
	for (const FVaRestJson& Event : Journal[TEXT("events")].Elements())
	{
		if (Event[TEXT("ev")].AsString().Equals(Kind, ESearchCase::CaseSensitive) && (ConnIndex == 0 || Event[TEXT("conn")].AsInt() == ConnIndex))
		{
			Out.Add(Event);
		}
	}
	return Out;
}

FString VaRest::WsTest::JournalHeader(const FVaRestJson& Journal, const TCHAR* Name, int32 ConnIndex)
{
	// RequestEvent, not Request: the name would hide VaRest::WsTest::Request (C4459 is an error, review spec M-10)
	for (const FVaRestJson& RequestEvent : JournalEvents(Journal, TEXT("request"), ConnIndex))
	{
		for (const FVaRestJson& Pair : RequestEvent[TEXT("headers")].Elements())
		{
			// RFC 9110 5.1: header names ignore case
			if (Pair[0].AsString().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Pair[1].AsString();
			}
		}
	}
	return TEXT("(absent)");
}

int32 VaRest::WsTest::JournalCloseCode(const FVaRestJson& Journal, FString* OutReason, int32 ConnIndex)
{
	const TArray<FVaRestJson> Closes = JournalEvents(Journal, TEXT("close"), ConnIndex);
	if (Closes.Num() == 0)
	{
		return -1;
	}
	if (OutReason)
	{
		*OutReason = Closes[0][TEXT("reason")].AsString();
	}
	return Closes[0][TEXT("code")].AsInt(-1);
}

FString VaRest::WsTest::Utf8(TConstArrayView<uint8> Bytes)
{
	return Bytes.Num() > 0 ? FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num()) : FString();
}

FString VaRest::WsTest::Sha256Hex(TConstArrayView<uint8> Bytes)
{
	return VaRest::Http::FSha256::Hash(Bytes).ToHex();
}

#endif // WITH_DEV_AUTOMATION_TESTS
