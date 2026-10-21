// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Async/Mutex.h"

class IFileHandle;

namespace VaRest::Http
{
	/**
	 * Receives a response body on the transport's thread (IHttpRequest::SetResponseBodyReceiveStreamDelegateV2,
	 * IHttpRequest.h:273-284). In memory it reserves Content-Length + 1 on the first chunk, so FVaRestJson::ParseOwned
	 * appends its terminator without reallocating (spec 2.1) -- the engine's own buffer is reserved at exactly
	 * Content-Length (CurlHttp.cpp:631-633). The up-front reserve is capped at max(MaxBytes, ReserveCap) + 1 and the
	 * buffer grows past it, always keeping Num + 1 <= Max (reviews M-6, M13). With MaxBytes (a JSON result's cap) the
	 * write that passes it fails, which stops the transfer (IHttpRequest.cpp:36-44).
	 *
	 * For SaveTo it writes a file instead, created with its directory on the first byte. Those writes run on the HTTP
	 * thread, as the engine's own SetResponseBodyReceiveStream to a file archive does (decision 31): a slow disk stalls
	 * the shared HTTP thread. A bounded pipe could go behind this class later without an API change.
	 *
	 * Locked (review C-4): on Apple, Write runs on NSURLSession's delegate queue (AppleHttp.cpp:194-206, :884-888) while
	 * the completion reads on the HTTP thread, and the engine orders the two only through a plain property
	 * (AppleHttp.cpp:220-233, :948-952). Write runs inside the engine's stream lock (HttpRequestCommon.cpp:711-723), so the
	 * lock order is engine stream lock -> Mutex, never the reverse. TakeBytes and CloseFile seal the sink: a later Write
	 * fails. Uncontended, the lock costs about 20 ns per curl chunk of 16 KiB or more.
	 */
	class FBodySink
	{
	public:
		/** The up-front reserve without a larger JSON cap: 8 MiB */
		static constexpr int64 ReserveCap = 8 * 1024 * 1024;

		FBodySink() = default;
		/** MaxBytes: the result's JSON cap (0 = none) */
		explicit FBodySink(int64 InMaxBytes);
		explicit FBodySink(FString InFilePath, int64 InMaxBytes = 0);
		~FBodySink();

		/**
		 * Appends Length bytes. ContentLength: the response's, or 0 or less when unknown. False on failure: a file write,
		 * over 2 GiB in memory, past MaxBytes, or after the sink was sealed.
		 */
		bool Write(const void* Data, int64 Length, int64 ContentLength);
		/** Memory: the bytes, with at least one byte of slack. Seals the sink. */
		TArray<uint8> TakeBytes();
		/** File: flushes and closes it; false when any write failed. Seals the sink. */
		bool CloseFile();

		int64 Num() const;
		bool IsFile() const { return !FilePath.IsEmpty(); }
		bool HasError() const;
		/** The body passed MaxBytes */
		bool IsTooLarge() const;
		/** Tests: the memory buffer and its capacity, to prove what is reserved */
		const uint8* GetDataInternal() const;
		int32 GetCapacityInternal() const;

	private:
		mutable UE::FMutex Mutex;
		FString FilePath;
		int64 MaxBytes = 0;
		TUniquePtr<IFileHandle> File;
		TArray<uint8> Bytes;
		int64 Written = 0;
		bool bError = false;
		bool bTooLarge = false;
		bool bSealed = false;
	};
}
