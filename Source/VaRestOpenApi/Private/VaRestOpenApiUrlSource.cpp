// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiUrlSource.h"
#include "VaRestOpenApiUrlPolicy.h"
#include "VaRestOpenApiUrlTransport.h"
#include "VaRestOpenApiUserSettings.h"
#include "VaRestApiClient.h"
#include "VaRestHub.h"
#include "VaRestConnectivity.h"
#include "Misc/ScopeExit.h"

namespace VaRest::OpenApi
{
namespace
{
TWeakObjectPtr<UVaRestHub> SourceHub;
TWeakObjectPtr<UVaRestApiClient> SourceWrapper;
TArray<TWeakPtr<FUrlSourceSession,ESPMode::ThreadSafe>> Sessions;
UVaRestApiClient* SourceClientOnGT()
{
	check(IsInGameThread());auto* Hub=UVaRestHub::GetForEditor();if (!Hub) return nullptr;
	if (SourceHub.Get()!=Hub || !SourceWrapper.IsValid() || !SourceWrapper->GetNativeClient().IsValid())
	{
		// One client per real engine Hub, not one retained Hub wrapper per document/import.
		SourceWrapper=Hub->MakeClient(TEXT("https://varest-source.invalid"));SourceHub=Hub;
		if (auto* Wrapper=SourceWrapper.Get()) Wrapper->GetNativeClient().WithConnectivity(FVaRestConnectivity::Make(FVaRestRealClock::Get()));
	}
	return SourceWrapper.Get();
}
}
struct FUrlSourceSession::FFetch
{
	std::atomic<bool> Done{false};TPromise<FVaRestOpenApiReadResult> Promise;
	void Set(FVaRestOpenApiReadResult&& Result) { if (!Done.exchange(true,std::memory_order_acq_rel)) Promise.SetValue(MoveTemp(Result)); }
};
TSharedRef<FUrlSourceSession,ESPMode::ThreadSafe> NewUrlSourceSession()
{
	check(IsInGameThread());Sessions.RemoveAll([](const auto& Session) { return !Session.IsValid(); });auto Session=MakeShared<FUrlSourceSession,ESPMode::ThreadSafe>();Sessions.Add(Session);return Session;
}
bool FUrlSourceSession::Read(const FVaRestOpenApiReadRequest& Request,FVaRestOpenApiReadResult& Out)
{
	check(!IsInGameThread());if (IsCancelled()) { Out.Error=TEXT("Source fetch cancelled.");return false; }
	FSourceUrl Url;FString Error;if (!AllowSourceUrl(Request,Url,Error)) { Out.Error=MoveTemp(Error);return false; }
	auto Fetch=MakeShared<FFetch,ESPMode::ThreadSafe>();auto Future=Fetch->Promise.GetFuture();auto Canonical=Request;Canonical.Uri=Url.Uri;
	AsyncTask(ENamedThreads::GameThread,[Self=AsShared(),Canonical=MoveTemp(Canonical),Fetch]() mutable { Self->StartOnGT(MoveTemp(Canonical),Fetch); });
	Out=Future.Get();return Out.Error.IsEmpty();
}
void FUrlSourceSession::StartOnGT(FVaRestOpenApiReadRequest Request,TSharedRef<FFetch,ESPMode::ThreadSafe> Fetch)
{
	check(IsInGameThread());if (IsCancelled()) { FVaRestOpenApiReadResult Result;Result.Error=TEXT("Source fetch cancelled.");Fetch->Set(MoveTemp(Result));return; }
	auto* Wrapper=SourceClientOnGT();if (!Wrapper) { FVaRestOpenApiReadResult Result;Result.Error=TEXT("The editor engine Hub could not admit a source client.");Fetch->Set(MoveTemp(Result));return; }
	auto Client=Wrapper->GetNativeClient();auto Status=MakeShared<FSourceFetchStatus,ESPMode::ThreadSafe>();
	const auto Headers=GetDefault<UVaRestOpenApiUserSettings>()->HeadersFor(Request.Uri);TArray<FString> Redacted;for (const auto& Header:Headers) Redacted.Add(Header.Name);
	Client.WithTransport(SourceTransport(Request,Status)).WithRedactedKeys(Redacted);
	ON_SCOPE_EXIT { Client.WithTransport(VaRest::Http::GetDefaultTransport()); };
	auto Builder=Client.Get<FVaRestResponse>(Request.Uri).Timeout(30.f).NoAuth().NoRetry().NoCache().NoEnvelope().OnHttpThread()
		.Header(TEXT("Accept"),TEXT("application/json, application/yaml;q=0.9, */*;q=0.5"));
	for (const auto& Header:Headers) Builder=MoveTemp(Builder).Header(Header.Name,Header.Value);
	const FString Uri=Request.Uri;
	auto Handle=MoveTemp(Builder)
		.OnSuccess([Fetch,Status,Uri](const FVaRestResponse& Response)
		{
			FVaRestOpenApiReadResult Result;Status->Read(Result.Error,Result.CanonicalUri);if (Result.CanonicalUri.IsEmpty()) Result.CanonicalUri=Uri;
			if (Result.Error.IsEmpty()) Result.Bytes=Response.Body;Fetch->Set(MoveTemp(Result));
		})
		.OnError([Fetch,Status](const FVaRestError& Error)
		{
			FVaRestOpenApiReadResult Result;Status->Read(Result.Error,Result.CanonicalUri);if (Result.Error.IsEmpty()) Result.Error=Error.Message.IsEmpty() ? TEXT("The source HTTP request failed.") : Error.Message;Fetch->Set(MoveTemp(Result));
		}).Send();
	if (Handle.GetId()==0) { FVaRestOpenApiReadResult Result;Result.Error=TEXT("The source client refused request admission.");Fetch->Set(MoveTemp(Result)); }
	Pending.RemoveAll([](const FPending& Existing) { return Existing.Fetch->Done.load(std::memory_order_acquire); });
	if (!Fetch->Done.load(std::memory_order_acquire)) Pending.Add({Handle,Fetch});
}
void FUrlSourceSession::CancelOnGT()
{
	check(IsInGameThread());Cancelled.store(true,std::memory_order_release);auto Cancelling=MoveTemp(Pending);
	for (const auto& Request:Cancelling)
	{
		// A shutdown may suppress core user callbacks. The source worker must still leave its own wait.
		FVaRestOpenApiReadResult Result;Result.Error=TEXT("Source fetch cancelled.");Request.Fetch->Set(MoveTemp(Result));Request.Handle.Cancel();
	}
}
void CancelUrlSourcesOnGT()
{
	check(IsInGameThread());for (const auto& Weak:Sessions) if (auto Session=Weak.Pin()) Session->CancelOnGT();Sessions.Reset();SourceWrapper.Reset();SourceHub.Reset();
}
FLocalSourceAnalysis AnalyzeUrlSource(const FString& Uri,const TSharedRef<FUrlSourceSession,ESPMode::ThreadSafe>& Session)
{
	FLocalSourceAnalysis Result;const auto Url=FSourceUrl::Parse(Uri);if (!Url.IsSet()) { FVaRestOpenApiIssue Issue;Issue.Code=TEXT("OA-DOC-READ");Issue.Document=Uri;Issue.Message=TEXT("Use an absolute HTTP(S) source URL without user-info credentials.");Result.Analysis.Issues.Add(MoveTemp(Issue));return Result; }
	FVaRestOpenApiReadRequest Root;Root.Uri=Url.GetValue().Uri;Root.RootBase=Url.GetValue().Base;Root.bUrl=true;FVaRestOpenApiReadResult Read;
	if (!Session->Read(Root,Read)) { FVaRestOpenApiIssue Issue;Issue.Code=TEXT("OA-DOC-READ");Issue.Document=Root.Uri;Issue.Message=Read.Error;Result.Analysis.Issues.Add(MoveTemp(Issue));return Result; }
	if (!SniffOpenApi(Read.Bytes)) { FVaRestOpenApiIssue Issue;Issue.Code=TEXT("OA-DOC-NOTOPENAPI");Issue.Document=Read.CanonicalUri;Issue.Message=TEXT("The fetched document has no top-level openapi or swagger key.");Result.Analysis.Issues.Add(MoveTemp(Issue));return Result; }
	auto Documents=FVaRestJson::Array();Documents.Add(SourceRecord(Read.CanonicalUri,Read.Bytes));FVaRestOpenApiLoadOptions Options;Options.bRetainSourceText=true;
	Options.Read=[&](const FVaRestOpenApiReadRequest& Request,FVaRestOpenApiReadResult& Out) { if (!Session->Read(Request,Out)) return false;Documents.Add(SourceRecord(Out.CanonicalUri,Out.Bytes));return true; };
	Result.Analysis=FVaRestOpenApiLoader::Analyze(Read.CanonicalUri,Read.Bytes,Options);
	Result.Source=FVaRestJson::Object().Set(TEXT("kind"),TEXT("url")).Set(TEXT("location"),Root.Uri).Set(TEXT("canonical"),Read.CanonicalUri).Set(TEXT("documents"),Documents).Set(TEXT("fetchedAt"),FDateTime::UtcNow().ToIso8601());return Result;
}
}
