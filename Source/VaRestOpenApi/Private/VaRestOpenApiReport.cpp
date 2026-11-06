// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiSource.h"
#include "VaRestOpenApiAnalysis.h"
#include "Logging/MessageLog.h"
#include "Misc/UObjectToken.h"

namespace VaRest::OpenApi
{
FVaRestJson IssueJson(TConstArrayView<FVaRestOpenApiIssue> Issues)
{
	auto Rows=FVaRestJson::Array();
	for (const auto& Issue:Issues) Rows.Add(FVaRestJson::Object().Set(TEXT("severity"),Issue.Severity==EVaRestOpenApiSeverity::Error ? TEXT("Error") : Issue.Severity==EVaRestOpenApiSeverity::Warning ? TEXT("Warning") : TEXT("Info"))
		.Set(TEXT("code"),Issue.Code).Set(TEXT("message"),Issue.Message).Set(TEXT("document"),Issue.Document).Set(TEXT("pointer"),Issue.Pointer).Set(TEXT("line"),Issue.Line).Set(TEXT("column"),Issue.Column).Set(TEXT("asset"),Issue.AssetPath));
	return Rows;
}
TArray<FVaRestOpenApiIssue> ReadIssueJson(const FVaRestJson& Rows)
{
	TArray<FVaRestOpenApiIssue> Issues;
	for (const auto& Row:Rows.Elements())
	{
		FVaRestOpenApiIssue Issue;const FString Severity=Row[TEXT("severity")].AsString();Issue.Severity=Equal(Severity,TEXT("Error")) ? EVaRestOpenApiSeverity::Error : Equal(Severity,TEXT("Warning")) ? EVaRestOpenApiSeverity::Warning : EVaRestOpenApiSeverity::Info;
		Issue.Code=Row[TEXT("code")].AsString();Issue.Message=Row[TEXT("message")].AsString();Issue.Document=Row[TEXT("document")].AsString();Issue.Pointer=Row[TEXT("pointer")].AsString();Issue.AssetPath=Row[TEXT("asset")].AsString();
		Issue.Line=Row[TEXT("line")].TryGet<int32>().Get(0);Issue.Column=Row[TEXT("column")].TryGet<int32>().Get(0);Issues.Add(MoveTemp(Issue));
	}
	return Issues;
}
void Report(TConstArrayView<FVaRestOpenApiIssue> Issues,bool bOpen)
{
	check(IsInGameThread());FMessageLog Log(TEXT("VaRestOpenAPI"));Log.NewPage(NSLOCTEXT("VaRestOpenApi","Report","OpenAPI import"));
	TStringMap<int32> InfoCounts;FStringSet Emitted;bool Attention=false;
	for (const auto& Issue:Issues) if (Issue.Severity==EVaRestOpenApiSeverity::Info) ++InfoCounts.FindOrAdd(Issue.Code);
	for (const auto& Issue:Issues)
	{
		if (Issue.Severity==EVaRestOpenApiSeverity::Info && Emitted.Contains(Issue.Code)) continue;
		Emitted.Add(Issue.Code);
		FString Text=Issue.Code+TEXT(": ")+Issue.Message;
		if (const auto* Count=InfoCounts.Find(Issue.Code); Issue.Severity==EVaRestOpenApiSeverity::Info && Count && *Count>1) Text+=FString::Printf(TEXT(" (%d occurrences)"),*Count);
		if (!Issue.Document.IsEmpty()) Text+=FString::Printf(TEXT("\n%s:%d:%d #%s"),*Issue.Document,Issue.Line,Issue.Column,*Issue.Pointer);
		const auto Severity=Issue.Severity==EVaRestOpenApiSeverity::Error ? EMessageSeverity::Error : Issue.Severity==EVaRestOpenApiSeverity::Warning ? EMessageSeverity::Warning : EMessageSeverity::Info;
		auto Message=Log.Message(Severity,FText::FromString(Text));
		if (!Issue.AssetPath.IsEmpty()) if (auto* Asset=FindObject<UObject>(nullptr,*Issue.AssetPath)) Message->AddToken(FUObjectToken::Create(Asset));
		Attention=Attention || Issue.Severity!=EVaRestOpenApiSeverity::Info;
	}
	if (bOpen && Attention) Log.Open(EMessageSeverity::Warning,true);
}
}
