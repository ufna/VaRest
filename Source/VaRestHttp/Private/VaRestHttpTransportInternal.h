// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Serialization/Archive.h"
#include "VaRestTransport.h"

#include <atomic>

namespace VaRest::Http
{
	/**
	 * Decision 30 (review I-5): the activity timeout the transport sets. With a total timeout and no activity timeout the
	 * engine would still apply its own default (GetActivityTimeoutOrDefault -> [HTTP] HttpActivityTimeout = 30,
	 * HttpRequestCommon.cpp:733-736, HttpModule.cpp:98), so .Timeout(60) against a server silent for 40 s would fail at 30 s
	 * as a connection error. Spec 4.7: the request's total timeout governs. 0 = set none.
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
}
