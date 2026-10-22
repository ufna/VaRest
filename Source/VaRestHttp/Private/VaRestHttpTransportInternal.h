// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Async/Mutex.h"
#include "Async/UniqueLock.h"
#include "HAL/PlatformTime.h"
#include "Serialization/Archive.h"
#include "VaRestBodySink.h"
#include "VaRestHttpTypes.h"
#include "VaRestTransport.h"

#include <atomic>

namespace VaRest::Http
{
	/**
	 * Decision 30 (review I-5): the activity timeout the transport sets. With a total timeout and no activity timeout the
	 * engine would still apply its own default (GetActivityTimeoutOrDefault -> [HTTP] HttpActivityTimeout = 30,
	 * HttpRequestCommon.cpp:733-736, HttpModule.cpp:98), so .Timeout(60) against a server silent for 40 s would fail at 30 s
	 * as a connection error. Spec 4.7: the request's total timeout governs. 0 = set none.
	 *
	 * Apple caps it (task 7 review M-9): NSURLSession's timeoutInterval is [HTTP] HttpConnectionTimeout (30 s,
	 * BaseEngine.ini:40; AppleHttp.cpp:736-738), so silence there still ends at 30 s, and an activity timeout above it
	 * logs "HttpConnectionTimeout can't be less than HttpActivityTimeout" as a Warning on every such request
	 * (AppleHttp.cpp:740-749) -- .Timeout(60) without an activity timeout is one. docs/dev/http-thread-completion.md
	 * (Task 17) records both.
	 */
	float EffectiveActivityTimeout(float TotalSeconds, float ActivitySeconds);

	/**
	 * Decision 29 (reviews N-1, N-9): whether an upload goes through FCountingUpload. Never on Apple (PLATFORM_APPLE,
	 * UEBuildApple.cs:239): NSURLSession reports upload progress itself (didSendBodyData -> TickThreadedRequest ->
	 * OnRequestProgress64, AppleHttp.cpp:161-168, :954-962, :899-911), and a body stream there cannot be resent (HTTPBodyStream,
	 * AppleHttp.cpp:627-634, :751-753; the engine implements no needNewBodyStream), so a 307/308 or an auth challenge would
	 * fail. On curl a progress callback turns it on (the caller asked; OnProgress documents the cost). Trace metadata alone
	 * turns it on only when curl could not rewind an in-memory body anyway (bCurlMayRewind false: [HTTP.Curl]
	 * bAllowSeekFunction, false by default, BaseEngine.ini:56; CurlHttp.cpp:1051), so attaching a trace sink never changes
	 * a request's outcome. Nothing to send, nothing to count.
	 */
	bool ShouldCountUpload(int64 UploadTotal, bool bProgressCallback, bool bReportProgress, bool bCurlMayRewind);

	/** [HTTP.Curl] bAllowSeekFunction, as the engine reads it (CurlHttpManager.cpp:304; re-read on config updates, :502-511). Game thread. */
	bool CurlMayRewindBodies();

	/**
	 * Task 2 review minor 3: what the PreCheck mirror hands FHttpManager::IsDomainAllowed -- the URL up to the end of its
	 * authority (scheme://host[:port]), or the whole URL when it has no "://". The filter splits at the first "://" and
	 * decides on the scheme and on the domain up to the first of "/?:#" after it (URLRequestFilter.cpp:95-128, :131-160),
	 * so the prefix gets the full URL's answer. IsDomainAllowed also logs the whole URL as a Warning when "://" appears
	 * twice (CheckNoDuplicatedProtocol, HttpManager.cpp:279-297); the engine's own PreCheck does that once per request
	 * (HttpRequestCommon.cpp:109), and the prefix, which ends at the first '/' after the first "://", cannot hold a second
	 * one: the mirror adds no second raw-URL line (an ApiKey in the query included).
	 */
	FString DomainFilterInput(const FString& Url);

	/**
	 * Decision 29, task 7 review I-1: whether the engine's progress delegate stamps UploadDoneAt (IN-3). Only for a request
	 * with a body -- with UploadTotal 0 the first progress call qualifies, and curl's first one comes with the first response
	 * bytes (CurlHttp.cpp:453-467), after StatusAt, which would make the Inspector's Wait phase negative -- and only where
	 * the backend reports upload progress itself (bBackendReportsUploadProgress: NSURLSession's didSendBodyData,
	 * AppleHttp.cpp:161-168, so PLATFORM_APPLE). curl's progress cannot see the upload end (UploadCallback reports nothing,
	 * CurlHttp.cpp:668-683), so on curl the stamp belongs to FCountingUpload alone: an uncounted curl upload (trace metadata
	 * with [HTTP.Curl] bAllowSeekFunction on, review N-9) leaves UploadDoneAt at -1, and the Inspector merges Send into Wait.
	 */
	bool ShouldStampUploadFromProgress(int64 UploadTotal, bool bBackendReportsUploadProgress);

	/**
	 * Decision 29 (review I-2): an upload that reports progress while it goes. Under CompleteOnHttpThread curl calls the
	 * progress delegate only from the body receive callback and at the finish (CurlHttp.cpp:453-467, :1274), and its
	 * upload callback reports nothing (CurlHttp.cpp:668-683): BytesSent stays 0 for a whole upload and UploadDoneAt would
	 * be stamped at the first response byte. So when progress is asked for (OnProgress, or trace metadata, IN-11, as
	 * ShouldCountUpload decides) the body goes through this archive and IHttpRequest::SetContentFromStream.
	 *
	 * Engine facts (UE 5.8.1): curl reads a stream payload through CURLOPT_READFUNCTION -> UploadCallback ->
	 * FRequestPayloadInFileStream::FillOutputBuffer (CurlHttp.cpp:421-429, :668-683, :996-1001; HttpRequestPayload.cpp:77-97),
	 * which serializes straight into curl's buffer on the HTTP thread (IHttpRequest.h:250-257, the note at :252): the same one copy an
	 * in-memory payload makes. Its length is TotalSize() (HttpRequestPayload.cpp:53-56), sent as Content-Length
	 * (CurlHttp.cpp:896-902) and CURLOPT_POSTFIELDSIZE/INFILESIZE (:952, :962, :987): no chunked encoding. A stream is not
	 * seekable (:390) where memory is (:348-349), but curl gets a seek callback only with [HTTP.Curl] bAllowSeekFunction,
	 * false by default (:1051, BaseEngine.ini:56), so by default a rewind fails the same way either way; with it on, only
	 * a progress callback still counts (review N-9, ShouldCountUpload). A streamed POST or DELETE needs a
	 * Content-Type (:945, :983); Resolve always sets one for a body (Task 8) and the transport stage refuses a missing or empty one
	 * (Task 8, reviews I3, N-5). Curl only: on Apple ShouldCountUpload is false (review N-1).
	 *
	 * Review N-4: the engine ignores the archive's error (FillOutputBuffer, HttpRequestPayload.cpp:86-96), so a read that
	 * fails would send whatever curl's buffer held. Every failed read zeroes the buffer and sets HasReadFailed(), and the
	 * completion then fails the request with UploadReadFailed (Kind=Config), whatever the server answered. The size is the
	 * inner archive's TotalSize(), or, for a file, the size measured when the body was built: a file whose size changed
	 * since fails its first read, so Content-Length and the bytes never disagree.
	 *
	 * Review N-8: UploadDoneAt is stamped when the reader takes the last byte, not when it leaves the socket.
	 *
	 * One reader at a time (curl's read callback); the probe's atomics and HasReadFailed are read on other threads. A file
	 * is opened on the first read, on the reading thread, never on the game thread.
	 */
	class FCountingUpload final : public FArchive
	{
	public:
		using FProbeRef = TSharedRef<FVaRestTransportProbe, ESPMode::ThreadSafe>;
		using FClockPtr = TSharedPtr<IVaRestClock, ESPMode::ThreadSafe>;

		FCountingUpload(TArray<uint8>&& InBytes, FProbeRef InProbe, FClockPtr InClock, bool bInStamp);
		/** The size is InInner's TotalSize() (review N-4) */
		FCountingUpload(TSharedRef<FArchive, ESPMode::ThreadSafe> InInner, FProbeRef InProbe, FClockPtr InClock, bool bInStamp);
		/** InTotal: the file's size when the body was built; a different size at the first read fails it (review N-4) */
		FCountingUpload(FString InFilePath, int64 InTotal, FProbeRef InProbe, FClockPtr InClock, bool bInStamp);
		virtual ~FCountingUpload() override;

		/** A read failed; the engine sent zeroes in its place (review N-4). Any thread. */
		bool HasReadFailed() const { return bReadFailed.load(); }

		virtual void Serialize(void* Data, int64 Length) override;
		virtual void Seek(int64 InPos) override;
		virtual int64 Tell() override { return Position; }
		virtual int64 TotalSize() override { return Total; }
		virtual FString GetArchiveName() const override { return TEXT("VaRest upload"); }

	private:
		void Count(int64 Length);
		/** Zeroes what the engine will send anyway, sets the archive error and HasReadFailed */
		void FailRead(void* Data, int64 Length);

		TArray<uint8> Bytes;
		TSharedPtr<FArchive, ESPMode::ThreadSafe> Inner;
		FString FilePath;
		TUniquePtr<FArchive> File;
		int64 Total = 0;
		int64 Position = 0;
		FProbeRef Probe;
		FClockPtr Clock;
		bool bStamp = false;
		std::atomic<bool> bReadFailed{ false };
	};

	/**
	 * The state the engine delegates of one FHttpModule request share (task 7 review M-6: here, not in the .cpp, so its
	 * fallback is unit-tested). Completes exactly once: the completion delegate, a failure before ProcessRequest and the
	 * destructor all go through Claim().
	 */
	struct FHttpTransportCompletion
	{
		FHttpTransportCompletion(FVaRestTransportComplete&& InOnComplete, TSharedRef<FBodySink, ESPMode::ThreadSafe> InSink,
			TSharedRef<FVaRestTransportProbe, ESPMode::ThreadSafe> InProbe, TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> InClock)
			: OnComplete(MoveTemp(InOnComplete))
			, Sink(MoveTemp(InSink))
			, Probe(MoveTemp(InProbe))
			, Clock(MoveTemp(InClock))
		{
		}

		/**
		 * Review I-3: the engine does not always call back. FHttpManager::Shutdown calls Request->Shutdown() on every
		 * outstanding request (HttpManager.cpp:164-169), which unbinds the delegates (HttpRequestImpl.cpp:32-38); with [HTTP]
		 * bUseNullHttp, CreateRequest returns a request that never completes (HttpModule.cpp:377-380, NullHttp.cpp:142-149).
		 * Either way this object dies with the request: finish it, with Failure Other, detail TransportDropped.
		 *
		 * Where it runs (task 7 review M-6): on the thread that drops the engine's last delegate. With NullHttp that is the
		 * game thread at the end of Start, no engine lock held. At HTTP module shutdown it is the game thread inside
		 * FHttpManager::Shutdown, holding FHttpManager::RequestLock (HttpManager.cpp:158-173) and the request's
		 * ResponseBodyReceiveStreamCriticalSection: the stream delegate goes last (FHttpRequestCommon::Shutdown,
		 * HttpRequestCommon.cpp:596-601; StopPassingReceivedData, :725-730). Both are recursive (FCriticalSection,
		 * HAL/CriticalSection.h:53), so OnComplete may post work -- a task, an atomic -- and nothing else: it must not call
		 * FHttpModule or any request (Shutdown is iterating the list that would change), block, wait for a thread that takes
		 * either lock, or run user code. The core's OnComplete only posts a game-thread task (IVaRestTransport::Start), which
		 * at shutdown never runs.
		 */
		~FHttpTransportCompletion()
		{
			if (FVaRestTransportComplete Last = Claim())
			{
				FVaRestTransportResult Dropped;
				Dropped.Failure = EVaRestTransportFailure::Other;
				Dropped.FailureDetail = TEXT("TransportDropped");
				Last(MoveTemp(Dropped));
			}
		}

		/** OnComplete for the first caller (any thread); an empty function for every later one */
		FVaRestTransportComplete Claim()
		{
			return bDone.exchange(true) ? FVaRestTransportComplete() : MoveTemp(OnComplete);
		}

		bool IsDone() const { return bDone.load(); }

		double Now() const
		{
			return Clock.IsValid() ? Clock->Now() : FPlatformTime::Seconds();
		}

		/**
		 * Plan decision 14 (final review M2): one header line of the final response, as OnHeaderReceived reports it -- on curl
		 * once per line, repeats apart, a followed redirect's skipped (CurlHttp.cpp:509-535, :556-560); on Apple the folded
		 * dictionary replayed (HttpRequestImpl.cpp:40-60). Any thread: curl's HTTP thread, Apple's session delegate queue
		 * (didReceiveResponse, AppleHttp.cpp:171-187, :865-874), or the game thread under the spec 9 fallback, where curl
		 * broadcasts what is left before it completes (CurlHttp.cpp:1305-1308) and Apple replays all of them in one game-thread
		 * task (:875-881). The completion takes them under the same lock: every line, or none and the engine's folded list.
		 */
		void AddHeader(const FString& Name, const FString& Value)
		{
			UE::TUniqueLock<UE::FMutex> Lock(HeadersMutex);
			CollectedHeaders.Add(Name, Value);
		}

		/** What AddHeader collected, once; empty when the delegate never ran (an engine mock response, review M-15) */
		FVaRestHeaders TakeHeaders()
		{
			UE::TUniqueLock<UE::FMutex> Lock(HeadersMutex);
			return MoveTemp(CollectedHeaders);
		}

		FVaRestTransportComplete OnComplete;
		TSharedRef<FBodySink, ESPMode::ThreadSafe> Sink;
		TSharedRef<FVaRestTransportProbe, ESPMode::ThreadSafe> Probe;
		TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> Clock;
		/** The response named a Content-Encoding (set by the header delegate, read by the first body chunk; review M-16) */
		std::atomic<bool> bEncodedBody{ false };
		/** Stamp transport times (trace metadata, IN-11) */
		bool bStamp = false;
		/** The progress delegate stamps UploadDoneAt (ShouldStampUploadFromProgress, review I-1) */
		bool bStampUploadFromProgress = false;
		/** The progress delegate is bound: BytesUp is then what it saw sent (review M-18, task 7 review M-4) */
		bool bProgressBound = false;
		/**
		 * Set when the body goes through FCountingUpload: BytesUp is then what was sent, not what was meant (review M-18),
		 * and a failed read fails the request (review N-4). The engine's payload holds it too; it holds no completion.
		 */
		TSharedPtr<FCountingUpload, ESPMode::ThreadSafe> CountedUpload;
		/**
		 * Set when segments go to the engine as they are (multipart, or a BodyFile; not counted): the engine ignores the
		 * stream's error (FillOutputBuffer, HttpRequestPayload.cpp:77-97), so a file that could not be read --
		 * FVaRestSegmentStream sends zeroes and sets HasReadFailed -- fails the request like a counted one (Task 3 review M4).
		 */
		TSharedPtr<FVaRestSegmentStream, ESPMode::ThreadSafe> SegmentUpload;
		int64 BytesUp = 0;

	private:
		std::atomic<bool> bDone{ false };
		UE::FMutex HeadersMutex;
		FVaRestHeaders CollectedHeaders;
	};
}
