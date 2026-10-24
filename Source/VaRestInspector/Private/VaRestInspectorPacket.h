// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Containers/ArrayView.h"
#include "Containers/StringView.h"
#include "Templates/SharedPointer.h"
#include "VaRestInspectorAllocation.h"
#include "VaRestInspectorPacketView.h"
#include "VaRestTrace.h"

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
using View::EText;
using View::FArraySlice;
using View::FPacketFixed;
using View::FPacketHeader;
using View::FPacketScalars;
using View::FPairSlice;
using View::FTextSlice;
enum class EMeasure : uint8
{
	Full,
	ScalarOnly,
	Unsupported
};
struct FPacketLayout
{
	static constexpr int32 MaxTexts = static_cast<int32>(EText::Count) + 2 * 256 + 2 * 32;
	FPacketFixed Fixed;
	FPairSlice Headers[256];
	FPairSlice Warnings[32];
	FStringView BorrowedText[MaxTexts];
	FTextSlice TextDestination[MaxTexts];
	int32 TextCount = 0;
	SIZE_T BackingRequest = 0, BackingBound = 0;
	int64 ExcerptCapacity = 0, PendingCharge = 0;
	bool bKeepExcerpt = false;
};
class FPacket
{
public:
	FPacket() = default;
	FPacket(FPacket&&) noexcept;
	FPacket& operator=(FPacket&&) noexcept;
	~FPacket();
	FPacket(const FPacket&) = delete;
	FPacket& operator=(const FPacket&) = delete;
	bool IsValid() const { return Backing != nullptr; }
	const FPacketFixed& Fixed() const;
	FStringView Text(EText Field) const;
	TConstArrayView<FPairSlice> Headers() const;
	TConstArrayView<FPairSlice> Warnings() const;
	FStringView Read(FTextSlice Slice) const;
	const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>& Excerpt() const;
	int64 RetainedCapacity() const;
	void Reset();

private:
	friend class FPacketCodec;
	friend class FStorePacketOwner;
	FPendingTicket Pending;
	FAllocationAdapter* Allocator = nullptr;
	void* Backing = nullptr;
	SIZE_T BackingCapacity = 0;
	SIZE_T BackingRequest = 0;
	TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> ExcerptOwner;
};
class FPacketCodec
{
public:
	static EMeasure Measure(const FVaRestTraceEvent& Borrowed,
		const FCapturePolicy& Policy, const FAllocationAdapter& Allocator,
		FPacketLayout& Out);
	static EMeasure MeasureStarted(const FVaRestTraceStarted& Borrowed,
		FPacketHeader Header, const FCapturePolicy& Policy,
		const FAllocationAdapter& Allocator, FPacketLayout& Out);
	static FPacket WriteStarted(const FVaRestTraceStarted& Borrowed,
		const FPacketLayout& Layout, FPendingTicket&& Reservation,
		FAllocationAdapter& Allocator);
	static FPacket Write(const FVaRestTraceEvent& Borrowed,
		const FPacketLayout& Layout, FPendingTicket&& Reservation,
		FAllocationAdapter& Allocator);

private:
	static FPacket WriteLayout(const FPacketLayout& Layout, FPendingTicket&& Reservation,
		FAllocationAdapter& Allocator,
		const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>* BorrowedExcerpt);
};
} // namespace VaRest::Inspector::Private
#endif
