// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspectorBodyViewModel.h"
#include "VaRestJsonRedact.h"

namespace
{
	TArray<TWeakPtr<FVaRestInspectorBodyViewModel>> BodyViews;
	FString BodyPath(FString Name)
	{ return TEXT("[") + FVaRestJson::From(Name).ToString() + TEXT("]"); }
	FString DisplayText(FString Text)
	{
		int32 Lines = 1, End = FMath::Min(Text.Len(), 1024 * 1024);
		for (int32 Index = 0; Index < End; ++Index) if (Text[Index] == TEXT('\n') && ++Lines > 20000) { End = Index; break; }
		return End < Text.Len() ? Text.Left(End) + TEXT("\n(display truncated)") : MoveTemp(Text);
	}
}
TSharedPtr<FVaRestInspectorBodyViewModel> FVaRestInspectorBodyViewModel::Create()
{
	check(IsInGameThread()); BodyViews.RemoveAll([](const auto& Weak) { return !Weak.IsValid(); });
	if (BodyViews.Num() >= 16) return {};
	TSharedPtr<FVaRestInspectorBodyViewModel> Out = MakeShareable(new FVaRestInspectorBodyViewModel()); BodyViews.Add(Out); return Out;
}
FVaRestInspectorBodyViewModel::~FVaRestInspectorBodyViewModel() { Shutdown(); }
void FVaRestInspectorBodyViewModel::ShutdownAll()
{
	check(IsInGameThread()); for (const auto& Weak : BodyViews) if (auto View = Weak.Pin()) View->Shutdown(); BodyViews.Reset();
}
void FVaRestInspectorBodyViewModel::Shutdown()
{
	check(IsInGameThread()); if (bStopped) return; bStopped = true; bNeedsJob = false;
	if (Job.IsValid()) Job.Wait(); // the bounded native parser never posts or waits for GTK
	Job = {}; Tree.Reset(); Current = {};
}
bool FVaRestInspectorBodyViewModel::SetSource(FVaRestInspectorBodySource Source)
{
	check(IsInGameThread()); if (bStopped) return false;
	if (Source.Bytes.Num() > 65536) { Source.Bytes.SetNum(65536); Source.bTruncated = true; }
	if (Source == Current && !RawText.IsEmpty()) return false;
	Current = MoveTemp(Source); if (Generation == MAX_uint64) { Shutdown(); return false; } ++Generation;
	Tree.Reset(); TreeNodes = 0; TreeBytes = 0; bPrettyJson = false;
	LimitText = FText::FromString(Current.bTruncated ? TEXT("Captured excerpt is truncated. Tree requires a complete JSON excerpt.") : Current.bRedacted ? TEXT("Redacted at capture; original secret values are unavailable.") : TEXT("Captured excerpt"));
	if (!Current.bAvailable) { RawText = PrettyText = HexText = FText::FromString(TEXT("(excerpt unavailable)")); bNeedsJob = false; return true; }
	const bool Binary = Current.Kind == EVaRestExcerptKind::Binary;
	RawText = FText::FromString(Binary ? (Current.bBinaryCaptured ? FString(TEXT("Binary excerpt; choose Hex to view captured bytes."))
		: FString::Printf(TEXT("Binary body: %lld bytes. Content capture is disabled."), Current.TotalBytes))
		: DisplayText(FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Current.Bytes.GetData()), Current.Bytes.Num())));
	FString Hex;
	if (Binary && Current.bBinaryCaptured)
	{
		const int32 Count = FMath::Min(Current.Bytes.Num(), 4096);
		for (int32 Index = 0; Index < Count; ++Index)
		{ Hex += FString::Printf(TEXT("%02x"), Current.Bytes[Index]); Hex += Index % 16 == 15 ? TEXT("\n") : TEXT(" "); }
		if (Count != Current.Bytes.Num()) Hex += TEXT("\n(hex display truncated)");
	}
	else Hex = TEXT("Hex is available only for opted-in binary excerpts.");
	HexText = FText::FromString(MoveTemp(Hex)); PrettyText = RawText;
	bNeedsJob = !Binary && (Current.Kind == EVaRestExcerptKind::Json || Current.Kind == EVaRestExcerptKind::Text);
	Start();
	return true;
}
void FVaRestInspectorBodyViewModel::Start()
{
	if (bStopped || !bNeedsJob || Job.IsValid()) return;
	bNeedsJob = false; JobGeneration = Generation;
	Job = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Source = Current]()
	{
		FParsed Out;
		FVaRestJsonParseOptions Options; Options.MaxInputBytes = 65536; Options.MaxDepth = 64; Options.MaxValues = 8192; Options.MaxWorkUnits = 1024 * 1024;
		auto Parsed = FVaRestJson::Parse(Source.Bytes, Options);
		if (Parsed.HasValue() && !Source.bTruncated)
		{ Out.Root = Parsed.StealValue(); Out.Pretty = Out.Root.ToString(EVaRestJsonFormat::Pretty); }
		else if (Source.bTruncated && Source.Kind == EVaRestExcerptKind::Json)
		{
			TArray<uint8> Pretty; const FVaRestRedactRules Rules;
			const int32 Count = FMath::Min(Source.Bytes.Num(), 8192);
			VaRest::Json::RedactPrefix(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Source.Bytes.GetData()), Count), Rules, EVaRestJsonFormat::Pretty, Pretty, false);
			Out.Pretty = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Pretty.GetData()), Pretty.Num()) + TEXT("\n(truncated JSON prefix; pretty prefix capped at 8 KiB; tree unavailable)");
		}
		else Out.Pretty = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Source.Bytes.GetData()), Source.Bytes.Num());
		Out.Pretty = DisplayText(MoveTemp(Out.Pretty));
		return Out;
	});
}
bool FVaRestInspectorBodyViewModel::Poll()
{
	check(IsInGameThread()); if (bStopped || !Job.IsValid() || !Job.IsCompleted()) return false;
	FParsed Parsed = MoveTemp(Job.GetResult()); Job = {}; const bool CurrentResult = JobGeneration == Generation;
	if (CurrentResult)
	{
		bPrettyJson = Parsed.Root.IsValid(); PrettyText = FText::FromString(MoveTemp(Parsed.Pretty));
		if (Parsed.Root.IsValid()) Tree.Add(Node(MoveTemp(Parsed.Root), TEXT("$"), TEXT("$"), 0));
	}
	Start(); return CurrentResult;
}
TSharedPtr<FVaRestInspectorBodyTreeNode> FVaRestInspectorBodyViewModel::Node(FVaRestJson Value, FString Name, FString Path, int32 Depth)
{
	if (TreeNodes >= 4096 || Path.Len() > 4096)
	{ LimitText = FText::FromString(TEXT("Tree display limit reached. Raw shows the captured excerpt.")); return {}; }
	const FString TextValue = Value.IsObject() ? FString::Printf(TEXT("{%d members}"), Value.Num())
		: Value.IsArray() ? FString::Printf(TEXT("[%d elements]"), Value.Num()) : Value.ToString().Left(256);
	const int64 Bytes = sizeof(FVaRestInspectorBodyTreeNode) + 128 + int64(Path.Len() + FMath::Min(Name.Len(), 256) + TextValue.Len() + 8) * sizeof(TCHAR);
	if (Bytes > 2 * 1024 * 1024 - TreeBytes)
	{ LimitText = FText::FromString(TEXT("Tree display limit reached. Raw shows the captured excerpt.")); return {}; }
	++TreeNodes; TreeBytes += Bytes;
	auto Out = MakeShared<FVaRestInspectorBodyTreeNode>(); Out->Value = MoveTemp(Value); Out->Path = MoveTemp(Path); Out->Depth = Depth;
	Out->Label = FText::FromString(Name.Left(256) + TEXT(": ") + TextValue); return Out;
}
void FVaRestInspectorBodyViewModel::Children(TSharedPtr<FVaRestInspectorBodyTreeNode> Parent, TArray<TSharedPtr<FVaRestInspectorBodyTreeNode>>& Out)
{
	check(IsInGameThread()); if (!Parent || bStopped) return;
	if (!Parent->bLoaded)
	{
		Parent->bLoaded = true;
		if (Parent->Depth < 64 && Parent->Value.IsObject())
		{
			for (const auto& Member : Parent->Value.Members())
			{
				if (Parent->Children.Num() >= 512 || TreeNodes >= 4096) break;
				const FString Key = Member.GetKey();
				if (auto Child = Node(Member.Value, Key, Parent->Path + BodyPath(Key), Parent->Depth + 1)) Parent->Children.Add(MoveTemp(Child));
			}
		}
		else if (Parent->Depth < 64 && Parent->Value.IsArray())
		{
			for (int32 Index = 0; Index < Parent->Value.Num() && Parent->Children.Num() < 512 && TreeNodes < 4096; ++Index)
				if (auto Child = Node(Parent->Value.At(Index), LexToString(Index), Parent->Path + FString::Printf(TEXT("[%d]"), Index), Parent->Depth + 1)) Parent->Children.Add(MoveTemp(Child));
		}
		if ((Parent->Value.IsObject() || Parent->Value.IsArray()) && Parent->Children.Num() < Parent->Value.Num())
			LimitText = FText::FromString(TEXT("Tree display limit reached. Raw shows the captured excerpt."));
	}
	Out = Parent->Children;
}
