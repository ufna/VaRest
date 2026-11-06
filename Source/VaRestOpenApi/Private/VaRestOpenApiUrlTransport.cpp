// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiUrlTransport.h"
#include "VaRestOpenApiUrlPolicy.h"
#include "VaRestOpenApiAnalysis.h"
#include "VaRestUrl.h"
#include "HttpModule.h"
#include "HttpManager.h"
#include "Interfaces/ISslCertificateManager.h"
#include "SslModule.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Tasks/Task.h"

THIRD_PARTY_INCLUDES_START
#include <curl/curl.h>
// OpenSSL's legacy UI typedef collides with CoreUObject's UI namespace in editor unity/PCH builds.
#pragma push_macro("UI")
#undef UI
#define UI VaRestOpenSslUI
#include <openssl/ssl.h>
#pragma pop_macro("UI")
THIRD_PARTY_INCLUDES_END

namespace VaRest::OpenApi
{
namespace
{
constexpr int64 SourceBytes=64ll*1024*1024;
constexpr int32 SourceRedirects=8;
bool RedirectStatus(int32 Code) { return Code==300 || Code==301 || Code==302 || Code==303 || Code==307 || Code==308; }
class FCurlCall final : public IVaRestTransportCall
{
public:
	FCurlCall(FVaRestTransportRequest&& InRequest,FVaRestTransportComplete&& InComplete,const FVaRestOpenApiReadRequest& InBoundary,TSharedRef<FSourceFetchStatus,ESPMode::ThreadSafe> InStatus,const ISslCertificateManager& InCertificates)
		:Request(MoveTemp(InRequest)),Complete(MoveTemp(InComplete)),Boundary(InBoundary),Status(MoveTemp(InStatus)),Certificates(InCertificates) {}
	virtual void Cancel() override { Cancelled.store(true,std::memory_order_release); }
	void Refuse(FName Detail,const FString& Error)
	{
		Status->SetError(Error);FVaRestTransportResult Result;Result.Failure=EVaRestTransportFailure::Other;Result.FailureDetail=Detail;Finish(MoveTemp(Result));
	}
	void Run();
private:
	static size_t Body(void* Data,size_t Unit,size_t Count,void* User);
	static size_t Header(char* Data,size_t Unit,size_t Count,void* User);
	static int Progress(void* User,curl_off_t,curl_off_t,curl_off_t,curl_off_t) { return static_cast<FCurlCall*>(User)->Cancelled.load(std::memory_order_acquire) ? 1 : 0; }
	static int ContextIndex() { static const int Index=SSL_CTX_get_ex_new_index(0,nullptr,nullptr,nullptr,nullptr);return Index; }
	static int Verify(int Allowed,X509_STORE_CTX* Context);
	static CURLcode SslContext(CURL*,void* Context,void* User);
	void Finish(FVaRestTransportResult&& Result)
	{
		if (Request.bReportStatusTime && Request.Clock) Result.TransportDoneAt=Request.Clock->Now();
		Request.Headers=FVaRestHeaders();if (Complete) { auto Callback=MoveTemp(Complete);Callback(MoveTemp(Result)); }
	}
	FVaRestTransportRequest Request;
	FVaRestTransportComplete Complete;
	FVaRestOpenApiReadRequest Boundary;
	TSharedRef<FSourceFetchStatus,ESPMode::ThreadSafe> Status;
	const ISslCertificateManager& Certificates;
	std::atomic<bool> Cancelled{false};
	FSourceUrl Current;
	FVaRestTransportResult Response;
	FString Location;
	int64 Received=0,HeaderBytes=0;
	bool TooLarge=false,HeadersTooLarge=false,InvalidHeaders=false,StoppedRedirect=false;
};
size_t FCurlCall::Body(void* Data,size_t Unit,size_t Count,void* User)
{
	auto& Call=*static_cast<FCurlCall*>(User);if (Unit!=0 && Count>MAX_uint64/Unit) return 0;const uint64 Bytes=Unit*Count;
	if (Call.Cancelled.load(std::memory_order_acquire)) return 0;
	if (Bytes>static_cast<uint64>(SourceBytes-Call.Received)) { Call.TooLarge=true;return 0; }
	Call.Received+=Bytes;Call.Request.Probe->BytesReceived.store(Call.Received,std::memory_order_relaxed);
	Call.Response.Body.Append(static_cast<const uint8*>(Data),static_cast<int32>(Bytes));return static_cast<size_t>(Bytes);
}
size_t FCurlCall::Header(char* Data,size_t Unit,size_t Count,void* User)
{
	auto& Call=*static_cast<FCurlCall*>(User);if (Unit!=0 && Count>MAX_uint64/Unit) return 0;const uint64 Bytes=Unit*Count;
	if (Bytes>128*1024 || Call.HeaderBytes+static_cast<int64>(Bytes)>128*1024) { Call.HeadersTooLarge=true;return 0; }
	Call.HeaderBytes+=Bytes;const FUTF8ToTCHAR Text(Data,static_cast<int32>(Bytes));FString Line(Text.Length(),Text.Get());Line.TrimEndInline();
	if (Line.StartsWith(TEXT("HTTP/"),ESearchCase::CaseSensitive))
	{
		int32 Space;if (Line.FindChar(TEXT(' '),Space)) Call.Response.Status=FCString::Atoi(*Line.Mid(Space+1,3));Call.Response.Headers=FVaRestHeaders();Call.Response.Body.Reset();Call.Location.Reset();
		if (Call.Request.bReportStatusTime && Call.Request.Clock) Call.Request.Probe->StatusAt.store(Call.Request.Clock->Now(),std::memory_order_relaxed);return static_cast<size_t>(Bytes);
	}
	if (Line.IsEmpty() && RedirectStatus(Call.Response.Status)) { Call.StoppedRedirect=true;return 0; } // no redirect body or next request is admitted yet
	FString Name,Value;if (Line.Split(TEXT(":"),&Name,&Value))
	{
		Value.TrimStartInline();if (!VaRest::Http::IsValidHeaderName(Name) || !VaRest::Http::IsValidHeaderValue(Value)) { Call.InvalidHeaders=true;return 0; }Call.Response.Headers.Add(Name,Value);
		if (Name.Equals(TEXT("Location"),ESearchCase::IgnoreCase)) Call.Location=Value;
		if (Name.Equals(TEXT("Content-Type"),ESearchCase::IgnoreCase)) Call.Response.ContentType=Value;
		if (Name.Equals(TEXT("Content-Length"),ESearchCase::IgnoreCase))
		{
			const int64 Length=FCString::Atoi64(*Value);if (Length>=0) Call.Request.Probe->DownloadTotal.store(Length,std::memory_order_relaxed);
			if (Length>SourceBytes && !RedirectStatus(Call.Response.Status)) { Call.TooLarge=true;return 0; }
		}
	}
	return static_cast<size_t>(Bytes);
}
int FCurlCall::Verify(int Allowed,X509_STORE_CTX* Context)
{
	if (!Allowed) return 0;auto* Handle=static_cast<SSL*>(X509_STORE_CTX_get_ex_data(Context,SSL_get_ex_data_X509_STORE_CTX_idx()));if (!Handle) return 0;
	auto* Call=static_cast<FCurlCall*>(SSL_CTX_get_ex_data(SSL_get_SSL_CTX(Handle),ContextIndex()));
	return Call && Call->Certificates.VerifySslCertificates(Context,Call->Current.Host) ? 1 : 0;
}
CURLcode FCurlCall::SslContext(CURL*,void* Context,void* User)
{
	auto* Call=static_cast<FCurlCall*>(User);auto* Ssl=static_cast<SSL_CTX*>(Context);if (ContextIndex()<0) return CURLE_SSL_CERTPROBLEM;
	Call->Certificates.AddCertificatesToSslContext(Ssl);SSL_CTX_set_ex_data(Ssl,ContextIndex(),Call);SSL_CTX_set_verify(Ssl,SSL_VERIFY_PEER,&Verify);return CURLE_OK;
}
void FCurlCall::Run()
{
	if (Cancelled.load(std::memory_order_acquire)) { Response.Failure=EVaRestTransportFailure::Canceled;Finish(MoveTemp(Response));return; }
	FString Error;if (!AllowSourceUrl(Boundary,Current,Error)) { Refuse(TEXT("DomainNotAllowed"),Error);return; }
	CURL* Curl=curl_easy_init();if (!Curl) { Refuse(TEXT("HttpDisabled"),TEXT("Could not create the source HTTP handle."));return; }
	curl_slist* Headers=nullptr;ON_SCOPE_EXIT { if (Headers) curl_slist_free_all(Headers);curl_easy_cleanup(Curl); };
	for (const auto& Pair:Request.Headers) { const FTCHARToUTF8 HeaderValue(*(Pair.Key+TEXT(": ")+Pair.Value));auto* Next=curl_slist_append(Headers,HeaderValue.Get());if (!Next) { Refuse(TEXT("InvalidHeader"),TEXT("Could not allocate the fetch headers."));return; }Headers=Next; }
	curl_easy_setopt(Curl,CURLOPT_HTTPHEADER,Headers);curl_easy_setopt(Curl,CURLOPT_HTTPGET,1L);curl_easy_setopt(Curl,CURLOPT_FOLLOWLOCATION,0L);
	curl_easy_setopt(Curl,CURLOPT_PROTOCOLS_STR,"http,https");curl_easy_setopt(Curl,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(Curl,CURLOPT_NETRC,static_cast<long>(CURL_NETRC_IGNORED));
	curl_easy_setopt(Curl,CURLOPT_SSL_VERIFYPEER,1L);curl_easy_setopt(Curl,CURLOPT_SSL_VERIFYHOST,2L);curl_easy_setopt(Curl,CURLOPT_SSL_CTX_FUNCTION,&SslContext);curl_easy_setopt(Curl,CURLOPT_SSL_CTX_DATA,this);
	curl_easy_setopt(Curl,CURLOPT_SUPPRESS_CONNECT_HEADERS,1L);curl_easy_setopt(Curl,CURLOPT_ACCEPT_ENCODING,"");
	curl_easy_setopt(Curl,CURLOPT_WRITEFUNCTION,&Body);curl_easy_setopt(Curl,CURLOPT_WRITEDATA,this);curl_easy_setopt(Curl,CURLOPT_HEADERFUNCTION,&Header);curl_easy_setopt(Curl,CURLOPT_HEADERDATA,this);
	curl_easy_setopt(Curl,CURLOPT_NOPROGRESS,0L);curl_easy_setopt(Curl,CURLOPT_XFERINFOFUNCTION,&Progress);curl_easy_setopt(Curl,CURLOPT_XFERINFODATA,this);
	const double Started=FPlatformTime::Seconds();const double Timeout=Request.Timeout>0 ? FMath::Min(30.0,static_cast<double>(Request.Timeout)) : 30.0;
	FStringSet Visited;
	for (int32 Redirect=0;;++Redirect)
	{
		const double Remaining=Timeout-(FPlatformTime::Seconds()-Started);
		if (Remaining<=0) { Response.Failure=EVaRestTransportFailure::Timeout;Response.FailureDetail=TEXT("TimedOut");break; }
		if (Cancelled.load(std::memory_order_acquire)) { Response.Failure=EVaRestTransportFailure::Canceled;break; }
		if (Visited.Contains(Current.Uri)) { Status->SetError(TEXT("The source URL redirects in a cycle."));Response.Failure=EVaRestTransportFailure::Other;Response.FailureDetail=TEXT("DomainNotAllowed");break; }Visited.Add(Current.Uri);
		Response=FVaRestTransportResult();Response.EffectiveUrl=Current.Uri;Location.Reset();HeaderBytes=0;StoppedRedirect=false;
		const FTCHARToUTF8 Url(*Current.Uri);curl_easy_setopt(Curl,CURLOPT_URL,Url.Get());curl_easy_setopt(Curl,CURLOPT_TIMEOUT_MS,FMath::Max(1L,static_cast<long>(Remaining*1000)));curl_easy_setopt(Curl,CURLOPT_CONNECTTIMEOUT_MS,FMath::Max(1L,static_cast<long>(Remaining*1000)));
		const CURLcode Code=curl_easy_perform(Curl);
		if (Cancelled.load(std::memory_order_acquire)) { Response.Failure=EVaRestTransportFailure::Canceled;break; }
		if (StoppedRedirect)
		{
			if (Redirect>=SourceRedirects) { Status->SetError(TEXT("The source exceeded eight redirects."));Response.Failure=EVaRestTransportFailure::Other;Response.FailureDetail=TEXT("DomainNotAllowed");break; }
			const auto Next=SourceRedirect(Boundary,Current.Uri,Location,Error);
			if (!Next.IsSet()) { Status->SetError(Error);Response.Failure=EVaRestTransportFailure::Other;Response.FailureDetail=TEXT("DomainNotAllowed");break; }
			Current=Next.GetValue();continue;
		}
		if (Code!=CURLE_OK)
		{
			if (TooLarge) { Response.Failure=EVaRestTransportFailure::Other;Response.FailureDetail=TEXT("ResponseTooLarge");Status->SetError(TEXT("The source response exceeds 64 MiB.")); }
			else if (HeadersTooLarge) { Response.Failure=EVaRestTransportFailure::Other;Response.FailureDetail=TEXT("ResponseBodyRejected");Status->SetError(TEXT("The source response headers exceed 128 KiB.")); }
			else if (InvalidHeaders) { Response.Failure=EVaRestTransportFailure::Other;Response.FailureDetail=TEXT("ResponseBodyRejected");Status->SetError(TEXT("The source response contains an invalid HTTP header.")); }
			else { Response.Failure=Code==CURLE_OPERATION_TIMEDOUT ? EVaRestTransportFailure::Timeout : EVaRestTransportFailure::Connection;Response.FailureDetail=Code==CURLE_PEER_FAILED_VERIFICATION || Code==CURLE_SSL_CONNECT_ERROR ? TEXT("SslFailure") : TEXT("SourceHttpFailure");Status->SetError(UTF8_TO_TCHAR(curl_easy_strerror(Code))); }
		}
		break;
	}
	Response.BytesDown=Received;Response.Body.Reserve(Response.Body.Num()+1);Status->SetUri(Current.Uri);Finish(MoveTemp(Response));
}
struct FJob { TWeakPtr<FCurlCall,ESPMode::ThreadSafe> Call;UE::Tasks::FTask Task; };
struct FWorkers { TArray<FJob> Jobs;bool Stopping=false,Initialized=false; };
FWorkers& Workers() { static FWorkers Value;return Value; } // touched only on GT; task captures own pure-native call
class FSourceTransport final : public IVaRestTransport
{
public:
	FSourceTransport(FVaRestOpenApiReadRequest InBoundary,TSharedRef<FSourceFetchStatus,ESPMode::ThreadSafe> InStatus):Boundary(MoveTemp(InBoundary)),Status(MoveTemp(InStatus)) {}
	virtual bool SendsBodyFor(FStringView) const override { return false; }
	virtual TSharedRef<IVaRestTransportCall,ESPMode::ThreadSafe> Start(FVaRestTransportRequest&& Request,FVaRestTransportComplete&& OnComplete) override
	{
		check(IsInGameThread());auto& Certificates=FSslModule::Get().GetCertificateManager();
		const bool IsSourceGet=Request.Verb.Equals(TEXT("GET"),ESearchCase::CaseSensitive) && Request.Body.IsEmpty() && Request.BodyFile.IsEmpty() && Request.BodySegments.IsEmpty() && Request.ResponseFile.IsEmpty();
		auto Actual=Boundary;Actual.Uri=Request.Url;auto Call=MakeShared<FCurlCall,ESPMode::ThreadSafe>(MoveTemp(Request),MoveTemp(OnComplete),Actual,Status,Certificates);auto& State=Workers();
		if (!IsSourceGet) { Call->Refuse(TEXT("BodyRejected"),TEXT("The source transport only accepts in-memory GET document fetches."));return Call; }
		if (State.Stopping || !FHttpModule::Get().IsHttpEnabled()) { Call->Refuse(TEXT("HttpDisabled"),TEXT("Source HTTP fetching is disabled or shutting down."));return Call; }
		FSourceUrl Url;FString Error;if (!AllowSourceUrl(Actual,Url,Error) || !FHttpModule::Get().GetHttpManager().IsDomainAllowed(Url.Origin)) { Call->Refuse(TEXT("DomainNotAllowed"),Error.IsEmpty() ? TEXT("The engine domain filter refused the source host.") : Error);return Call; }
		if (!State.Initialized) { if (curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) { Call->Refuse(TEXT("HttpDisabled"),TEXT("Could not initialize the source HTTP library."));return Call; }State.Initialized=true; }
		const auto* Version=curl_version_info(CURLVERSION_NOW);
		if (!Version || !Version->ssl_version || !FString(UTF8_TO_TCHAR(Version->ssl_version)).StartsWith(TEXT("OpenSSL"))) { Call->Refuse(TEXT("HttpDisabled"),TEXT("This source transport requires the engine OpenSSL curl backend."));return Call; }
		State.Jobs.RemoveAll([](const FJob& Job) { return Job.Task.IsCompleted(); });
		auto Task=UE::Tasks::Launch(TEXT("VaRest.OpenApi.SourceHttp"),[Call]() { Call->Run(); });State.Jobs.Add({Call,MoveTemp(Task)});return Call;
	}
private:
	FVaRestOpenApiReadRequest Boundary;
	TSharedRef<FSourceFetchStatus,ESPMode::ThreadSafe> Status;
};
}
TSharedRef<IVaRestTransport,ESPMode::ThreadSafe> SourceTransport(const FVaRestOpenApiReadRequest& Boundary,TSharedRef<FSourceFetchStatus,ESPMode::ThreadSafe> Status)
{
	return MakeShared<FSourceTransport,ESPMode::ThreadSafe>(Boundary,MoveTemp(Status));
}
void ShutdownSourceTransport()
{
	check(IsInGameThread());auto& State=Workers();State.Stopping=true;for (const auto& Job:State.Jobs) if (auto Call=Job.Call.Pin()) Call->Cancel();
	for (const auto& Job:State.Jobs) Job.Task.Wait();State.Jobs.Reset();if (State.Initialized) { curl_global_cleanup();State.Initialized=false; }
}
}
