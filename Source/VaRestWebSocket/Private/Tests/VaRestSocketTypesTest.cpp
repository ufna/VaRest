// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestJsonUtf8.h"
#include "VaRestSocketTypes.h"

namespace VaRest::WebSocket::VaRestSocketTypesTestPrivate
{
	FString PayloadText(const FVaRestSocketFrame& Frame)
	{
		const TConstArrayView<uint8> Payload = Frame.GetPayload();
		return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Payload.GetData()), Payload.Num());
	}

	bool IsUtf8(const TArray<uint8>& Bytes)
	{
		return VaRest::Json::IsValidUtf8(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num()));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketTypesFrameTest, "VaRest.WebSocket.Types.Frame",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketTypesFrameTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	using namespace VaRest::WebSocket::VaRestSocketTypesTestPrivate;
	const FVaRestSocketFrame Hello = FVaRestSocketFrame::Text(FUtf8StringView(UTF8TEXT("hello")));
	TestTrue(TEXT("a text frame is valid"), Hello.IsValid());
	TestTrue(TEXT("... and Text"), Hello.Kind == EVaRestSocketFrameKind::Text);
	TestEqual(TEXT("... its payload starts after the headroom"), Hello.Offset, FrameHeadroom);
	TestEqualSensitive(TEXT("... and is the text"), PayloadText(Hello), FString(TEXT("hello")));

	const FVaRestSocketFrame Cyrillic = FVaRestSocketFrame::Text(FStringView(TEXT("при")));
	TestEqual(TEXT("UTF-16 text becomes UTF-8 (three two-byte letters)"), Cyrillic.Length, 6);
	// An unpaired surrogate becomes U+FFFD, EF BF BD (VaRest::Json::WriteUtf8)
	const TCHAR Lone[] = { static_cast<TCHAR>(0xD800), TEXT('a'), 0 };
	const FVaRestSocketFrame Repaired = FVaRestSocketFrame::Text(FStringView(Lone, 2));
	const TArray<uint8> Expected = { 0xEF, 0xBF, 0xBD, 'a' };
	// TArrayView has no operator== in UE 5.8 (ArrayView.h names one only in comments): compare through TArray (Array.h:871, :1788)
	TestTrue(TEXT("an unpaired surrogate is sent as U+FFFD"), TArray<uint8>(Repaired.GetPayload()) == Expected);

	const uint8 Raw[] = { 0, 1, 2, 255 };
	const FVaRestSocketFrame Bytes = FVaRestSocketFrame::Binary(Raw);
	TestTrue(TEXT("a binary frame is Binary"), Bytes.Kind == EVaRestSocketFrameKind::Binary);
	TestTrue(TEXT("... with its bytes"), TArray<uint8>(Bytes.GetPayload()) == TArray<uint8>(Raw, UE_ARRAY_COUNT(Raw)));

	// SW-4: an encoder's own buffer, a larger gap before the payload
	TArray<uint8> Own;
	Own.Init(0xAA, 40);
	Own.Add('x');
	Own.Add('y');
	TOptional<FVaRestSocketFrame> Gap = FVaRestSocketFrame::FromStorage(EVaRestSocketFrameKind::Text, MoveTemp(Own), 40, 2);
	if (!TestTrue(TEXT("an offset past the headroom is taken"), Gap.IsSet()))
	{
		return false;
	}
	TestEqualSensitive(TEXT("... and the payload is what follows it"), PayloadText(Gap.GetValue()), FString(TEXT("xy")));
	TArray<uint8> Short;
	Short.SetNumZeroed(20);
	TestFalse(TEXT("an offset below the headroom is refused"), FVaRestSocketFrame::FromStorage(EVaRestSocketFrameKind::Text, MoveTemp(Short), 8, 4).IsSet());
	TArray<uint8> Over;
	Over.SetNumZeroed(20);
	TestFalse(TEXT("a length past the storage is refused"), FVaRestSocketFrame::FromStorage(EVaRestSocketFrameKind::Text, MoveTemp(Over), 16, 5).IsSet());
	TestFalse(TEXT("a default frame has no storage and is not valid"), FVaRestSocketFrame().IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketTypesCloseCodesTest, "VaRest.WebSocket.Types.CloseCodes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketTypesCloseCodesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	// RFC 6455 7.4.1-7.4.2 and the IANA registry; spec 3.5 and 2.5
	for (const int32 Code : { 1000, 1001, 1002, 1003, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 3000, 4000, 4999 })
	{
		TestTrue(FString::Printf(TEXT("%d may be sent and received"), Code), IsValidCloseCode(Code));
	}
	for (const int32 Code : { -1, 0, 999, 1004, 1005, 1006, 1015, 1016, 2000, 2999, 5000, 65535 })
	{
		TestFalse(FString::Printf(TEXT("%d may not"), Code), IsValidCloseCode(Code));
	}
	bool bReplaced = true;
	TestEqual(TEXT("4001 is sent as given"), SanitizeCloseCode(4001, bReplaced), 4001);
	TestFalse(TEXT("... unchanged"), bReplaced);
	TestEqual(TEXT("1006 becomes 1000 (spec 3.5)"), SanitizeCloseCode(1006, bReplaced), 1000);
	TestTrue(TEXT("... replaced"), bReplaced);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketTypesCloseReasonTest, "VaRest.WebSocket.Types.CloseReason",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketTypesCloseReasonTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	using namespace VaRest::WebSocket::VaRestSocketTypesTestPrivate;
	bool bTruncated = true;
	TestEqual(TEXT("a short reason is kept"), CloseReasonBytes(TEXT("bye"), bTruncated).Num(), 3);
	TestFalse(TEXT("... whole"), bTruncated);
	TestEqual(TEXT("200 ASCII characters are cut to 123 bytes"), CloseReasonBytes(FString::ChrN(200, TEXT('a')), bTruncated).Num(), MaxCloseReasonBytes);
	TestTrue(TEXT("... cut"), bTruncated);
	// Review spec I-1: a reason that fits is never cut, whatever its length in UTF-16 units
	TestEqual(TEXT("100 ASCII characters stay 100 bytes"), CloseReasonBytes(FString::ChrN(100, TEXT('a')), bTruncated).Num(), 100);
	TestFalse(TEXT("... and are not reported cut"), bTruncated);
	TestEqual(TEXT("123 ASCII characters stay 123 bytes"), CloseReasonBytes(FString::ChrN(MaxCloseReasonBytes, TEXT('a')), bTruncated).Num(), MaxCloseReasonBytes);
	TestFalse(TEXT("... and are not reported cut"), bTruncated);
	TestEqual(TEXT("124 are cut to 123"), CloseReasonBytes(FString::ChrN(MaxCloseReasonBytes + 1, TEXT('a')), bTruncated).Num(), MaxCloseReasonBytes);
	TestTrue(TEXT("... and reported cut"), bTruncated);
	// 70 two-byte letters, 140 bytes: 123 would split the 62nd, so 61 (122 bytes) are kept
	const TArray<uint8> Cyrillic = CloseReasonBytes(FString::ChrN(70, static_cast<TCHAR>(0x0430)), bTruncated);
	TestEqual(TEXT("a cut never splits a code point"), Cyrillic.Num(), 122);
	TestTrue(TEXT("... so the reason stays UTF-8"), IsUtf8(Cyrillic));
	TestTrue(TEXT("... and is reported cut"), bTruncated);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketTypesPeerCloseTest, "VaRest.WebSocket.Types.PeerClose",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketTypesPeerCloseTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	// Spec 2.5: length 0 -> 1005 (the engine reads two bytes anyway, E27)
	const FPeerClose Empty = ParsePeerClose(TConstArrayView<uint8>());
	TestEqual(TEXT("an empty close frame is 1005"), Empty.Code, 1005);
	TestEqualSensitive(TEXT("... with no reason"), Empty.Reason, FString());
	const uint8 One[] = { 0x03 };
	const FPeerClose OneByte = ParsePeerClose(One);
	TestEqual(TEXT("a one-byte close frame is 1002"), OneByte.Code, 1002);
	TestTrue(TEXT("... malformed"), OneByte.bMalformed);
	const uint8 Normal[] = { 0x03, 0xE8 };
	TestEqual(TEXT("03 E8 is 1000"), ParsePeerClose(Normal).Code, 1000);
	const uint8 Big[] = { 0x13, 0x88 };
	const FPeerClose Five = ParsePeerClose(Big);
	TestEqual(TEXT("5000, a code a peer must not send, becomes 1005"), Five.Code, 1005);
	TestTrue(TEXT("... normalised"), Five.bCodeNormalised);
	TestEqual(TEXT("... the received code kept"), Five.ReceivedCode, 5000);
	// close.reason_utf8_bytes: 4002 "пока ✓"
	const uint8 Bye[] = { 0x0F, 0xA2, 0xD0, 0xBF, 0xD0, 0xBE, 0xD0, 0xBA, 0xD0, 0xB0, 0x20, 0xE2, 0x9C, 0x93 };
	const FPeerClose Utf8 = ParsePeerClose(Bye);
	TestEqual(TEXT("4002 as sent"), Utf8.Code, 4002);
	TestEqualSensitive(TEXT("... the reason as UTF-8"), Utf8.Reason, FString(TEXT("пока ✓")));
	TestFalse(TEXT("... not repaired"), Utf8.bReasonRepaired);
	const uint8 Bad[] = { 0x03, 0xE8, 0xC3, 0x28 };
	const FPeerClose Repaired = ParsePeerClose(Bad);
	TestEqualSensitive(TEXT("ill-formed reason bytes become U+FFFD"), Repaired.Reason, FString(TEXT("�(")));
	TestTrue(TEXT("... repaired"), Repaired.bReasonRepaired);
	return true;
}

#endif
