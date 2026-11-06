// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Tasks/Pipe.h"
#include "VaRestRedactor.h"
#include "VaRestSocketTypes.h"
#include <atomic>

namespace VaRest::WebSocket
{
	EVaRestLogLevel SocketLogLevel(EVaRestLogLevel Configured);
	/** Scrub before bounding/escaping a field, so a truncated credential cannot escape value redaction. */
	FString SocketLogText(const FVaRestRedactor& Redactor, FStringView Text);
	class FSocketFrameLog;
	struct FSocketLogEntry;

	/** Native bounded copy prepared before a transport handoff. Destruction discards an unaccepted frame. */
	class FSocketFrameLogCapture final
	{
	public:
		FSocketFrameLogCapture();
		~FSocketFrameLogCapture();
		FSocketFrameLogCapture(FSocketFrameLogCapture&& Other);
		FSocketFrameLogCapture& operator=(FSocketFrameLogCapture&& Other);
		void SetMetadata(FStringView Channel, FStringView Route, FName ProtocolTag);
		void Publish();
	private:
		friend class FSocketFrameLog;
		TUniquePtr<FSocketLogEntry> Entry;
	};

	/** Pure native per-socket log pipe. No client, UObject, reflection, clock or user callback is retained. */
	class FSocketFrameLog final : public TSharedFromThis<FSocketFrameLog, ESPMode::ThreadSafe>
	{
	public:
		~FSocketFrameLog();
		void SetLevelOnGT(EVaRestLogLevel Level);
		FSocketFrameLogCapture Prepare(uint64 SocketId, uint64 AttemptId, bool bOutbound, EVaRestSocketFrameKind Kind,
			TConstArrayView<uint8> Bytes, TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> Redactor, int32 JsonOffset = -1, bool bRedactAllJsonValues = false);
		void StopAndWaitOnGT();
	private:
		friend class FSocketFrameLogCapture;
		friend struct FSocketLogEntry;
		void Publish(TUniquePtr<FSocketLogEntry> Entry);
		void Release(int32 Bytes);
		FCriticalSection Admission;
		UE::Tasks::FPipe Pipe{TEXT("VaRest.Socket.Log")};
		std::atomic<uint8> Level{static_cast<uint8>(EVaRestLogLevel::Basic)};
		bool bStopped = false;
		int32 Pending = 0, PendingBytes = 0;
		static constexpr int32 MaxPending = 256, MaxBytes = 1024 * 1024;
	};
}
