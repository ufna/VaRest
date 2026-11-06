// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestInspectorCurl.h"

namespace
{
	FString Quote(FString Text) { return TEXT("'") + Text.Replace(TEXT("'"), TEXT("'\\''")) + TEXT("'"); }
}
FString FVaRestInspectorCurl::Build(const FVaRestInspectorRecordView& Row, uint64 DispatchId)
{
	check(IsInGameThread());
	using VaRest::Inspector::View::EText;
	if (Row.Kind != EVaRestInspectorRecordKind::Http || !Row.Started.Fixed) return TEXT("# Request metadata is unavailable.\n");
	const FVaRestInspectorDispatchView* Attempt = nullptr;
	for (const auto& Item : Row.Dispatches) if (!DispatchId || Item.DispatchId == DispatchId) Attempt = &Item;
	if (DispatchId && !Attempt) return TEXT("# The selected attempt is no longer retained.\n");
	if (Attempt && Attempt->HeadersState != EVaRestInspectorHeadersState::Captured)
		return TEXT("# Final headers for this attempt are unavailable; a replay command cannot be constructed.\n");
	const auto Request = Attempt ? Attempt->Dispatched : Row.Started;
	const FString Url(Row.Started.Text(EText::Url).Left(8192)), Verb(Row.Started.Text(EText::Verb).Left(32));
	if (Url.IsEmpty() || Verb.IsEmpty()) return TEXT("# The captured request URL or method is unavailable.\n");
	FString Command = FString::Printf(TEXT("# VaRest Inspector #%llu, attempt %d; *** = redacted at capture\n"), Row.RequestId, Attempt ? Attempt->Attempt : 0);
	if (!Attempt) Command += TEXT("# As-built headers: no retained network attempt.\n");
	Command += TEXT("curl --url ") + Quote(Url) + TEXT(" -X ") + Quote(Verb);
	if (Request.Fixed) for (const auto& Pair : Request.Pairs(Request.Fixed->Headers))
	{
		const FString Name(Request.Read(Pair.First));
		if (Name.Equals(TEXT("Content-Length"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("Host"), ESearchCase::IgnoreCase)
			|| Name.Equals(TEXT("Connection"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("Transfer-Encoding"), ESearchCase::IgnoreCase)) continue;
		Command += TEXT(" \\\n  -H ") + Quote(Name + TEXT(": ") + FString(Request.Read(Pair.Second)));
	}
	if (Command.Len() > 131072) return TEXT("# Captured headers exceed the cURL copy limit. Export HAR instead.\n");
	bool FoundBody = false;
	for (const auto& Excerpt : Row.Excerpts)
	{
		if (!Excerpt.Fixed || Excerpt.Fixed->Scalars.Owner != EVaRestTraceExcerptOwner::Request) continue;
		const auto& Body = Excerpt.Fixed->Scalars;
		if (Body.ExcerptKind == EVaRestExcerptKind::Skipped) continue;
		FoundBody = true;
		if (Body.ExcerptKind == EVaRestExcerptKind::Binary)
			Command += TEXT(" \\\n  --data-binary @body.bin\n# Supply body.bin separately; captured binary bytes are not embedded.\n");
		else
		{
			const auto Bytes = Excerpt.Excerpt.Left(65536);
			bool Nul = false; for (uint8 Byte : Bytes) Nul |= Byte == 0;
			const FString Text = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
			if (Nul) return TEXT("# The excerpt contains NUL; save it and use --data-binary @body.bin.\n");
			Command += TEXT(" \\\n  --data-raw ") + Quote(Text);
		}
		if (Body.bTruncated || Excerpt.Excerpt.Num() > 65536) Command += TEXT("\n# Body is truncated; this is an incomplete replay.\n");
		break;
	}
	if (!FoundBody && Row.Started.Fixed->Scalars.BodyBytes > 0) Command += TEXT("\n# Request body was not captured; supply it before replay.\n");
	if (FoundBody && Attempt && Attempt->Attempt > 1) Command += TEXT("\n# Body excerpt was captured before the first attempt; verify any per-attempt changes.\n");
	return Command + TEXT("\n");
}
