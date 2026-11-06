// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once
#include "VaRestNativeOwnerAbi.h"
#include "VaRestNativeOwnerDeployment1.h"
#include "VaRestNativeOwnerPrivateApi1.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
#include <sys/stat.h>
#include <sys/vfs.h>
#include <type_traits>

namespace VaRestNativeOwnerPrivate::Deployment1
{
	// Same-image private read facts only. No record, index, mask or status grants
	// caller/loader authority. Q4 owns actual ActivationDepth and the sole scratch;
	// there are ZERO current callers. Full receipt/H/loader/kernel/return premises
	// remain prerequisites of any future call, not arguments that appoint them.
	struct FQReadFd1
	{
		int32_t Fd;
		uint32_t Custody;
		int32_t NativeError;
		uint32_t Kind;
		uint64_t AcquisitionOrdinal;
	};
	struct FQReadLedger1 { FQReadFd1 Cells[2]; uint64_t NextOrdinal; };
	// A genuine fresh frame initializes Cells to {-1,0,0,0,0}, NextOrdinal0.
	// No reset function exists. Caller ledger precedes/ends after every reader.
	enum : uint32_t { FdEmpty = 0, FdReserved = 1, FdKnown = 2,
		FdClosing = 3, FdClosed = 4, FdUncertain = 5,
		FdCatalogue = 1, FdOriginal = 2, FdSelfMaps = 3 };
	struct FQSha2561
	{
		uint32_t State[8];
		uint64_t Bytes;
		uint8_t Block[64];
		uint32_t Used, Reserved;
	};
	struct FQFileIdentity1
	{
		uint64_t Major, Minor, Inode, Bytes;
		int64_t ModifiedSeconds, ChangedSeconds;
		uint32_t ModifiedNanos, ChangedNanos;
	};
	struct FQLoad1 { uint64_t Start, End, FileOffset; uint32_t Flags, Reserved; };
	struct FQParsedLayout1
	{
		uint8_t Phdrs[16][56];
		FQLoad1 Loads[8];
		uint64_t Bias;
		uint32_t RecordIndex, Counts; // phdr count bits0..7, load count8..15
		VrBootstrap1 Bootstrap; // Q1 never invokes getter or fills callable fields.
	};
	struct FQImage1 { uint64_t Bias; uint32_t Classification, RecordIndex; };
	struct alignas(16) FQScratchStorage1
	{
		FQImage1 Images[4096];
		FQParsedLayout1 Candidates[32];
		uint8_t Paths[131072];
		uint8_t Catalogue[14592];
		uint8_t Io[4096];
	};
	struct FQMapParser1
	{
		uint64_t Start, End, Offset, Inode, Number, PreviousEnd;
		uint32_t Major, Minor, Digits, State, Permissions, LineBytes,
			PathBytes, PathMismatch, Lines, HasPath, Padding[2];
	};
	struct alignas(16) FQPhase1
	{
		struct stat Before, After;
		struct statfs FileSystem;
		FQSha2561 Sha;
		FQMapParser1 Map;
		uint64_t Coverage[8];
		uint8_t ElfHeader[64];
		uint64_t PageBytes, FileOffset, PayloadBytes;
		uint32_t Attempts, NoteBytes, FamilyNotes, BuildNotes;
	};
	static_assert(sizeof(FQReadFd1) == 24 && alignof(FQReadFd1) == 8);
	static_assert(offsetof(FQReadFd1, Fd) == 0 && offsetof(FQReadFd1, Custody) == 4);
	static_assert(offsetof(FQReadFd1, NativeError) == 8 && offsetof(FQReadFd1, Kind) == 12);
	static_assert(offsetof(FQReadFd1, AcquisitionOrdinal) == 16);
	static_assert(sizeof(FQReadLedger1) == 56 && sizeof(FQSha2561) == 112);
	static_assert(offsetof(FQSha2561, Bytes) == 32 && offsetof(FQSha2561, Block) == 40);
	static_assert(offsetof(FQSha2561, Used) == 104 && offsetof(FQSha2561, Reserved) == 108);
	static_assert(sizeof(FQFileIdentity1) == 56 && sizeof(FQMapParser1) <= 128);
	static_assert(sizeof(FQLoad1) == 32 && sizeof(FQParsedLayout1) == 1280 && sizeof(FQImage1) == 16);
	static_assert(offsetof(FQScratchStorage1, Candidates) == 65536);
	static_assert(offsetof(FQScratchStorage1, Paths) == 106496);
	static_assert(offsetof(FQScratchStorage1, Catalogue) == 237568);
	static_assert(offsetof(FQScratchStorage1, Io) == 252160);
	static_assert(sizeof(FQScratchStorage1) == 256256 && alignof(FQScratchStorage1) <= 16);
	static_assert(sizeof(struct stat) <= 256 && sizeof(FQPhase1) + 2 * sizeof(FQReadFd1) <= 1024);
	static_assert(std::is_trivial_v<FQReadFd1> && std::is_standard_layout_v<FQReadFd1>);
	static_assert(std::is_trivial_v<FQReadLedger1> && std::is_trivially_copyable_v<FQReadLedger1>);
	static_assert(std::is_trivial_v<FQSha2561> && std::is_standard_layout_v<FQSha2561>);
	static_assert(std::is_trivial_v<FQParsedLayout1> && std::is_standard_layout_v<FQParsedLayout1>);
	static_assert(std::is_trivial_v<FQScratchStorage1> && std::is_trivially_copyable_v<FQScratchStorage1>);
	static_assert(std::is_trivial_v<FQPhase1> && std::is_trivially_destructible_v<FQPhase1>);

	// All references are real aligned accessible nonaliasing same-image native
	// storage. Keep the same Phase.PageBytes from ReadOriginal through mapping.
	// Reader3/minimum-reader3 only. Historical API storage72 is known without
	// any tail read; only full14/storage216 can be selected before E. Original
	// file-backed and owned-mapping read/nonwrite checks cover its WHOLE extent.
	// Current source Profile2/140 ORIGINAL header32 consistency is additionally
	// read into Io4096 after layout/notes and before final fstat, charged to the
	// same original-attempt cap. It never inspects a cold mapped tail.
	// These read facts never establish actual tail fields, normal O, full receipt
	// qualification or caller authority; F1 neither reads nor emits a tail.
	[[gnu::visibility("hidden")]] VrStatus ReadCatalogue1(FQScratchStorage1&, FQPhase1&, FQReadLedger1&, uint32_t& Count, uint32_t& Selected);
	[[gnu::visibility("hidden")]] VrStatus ReadOriginal1(FQScratchStorage1&, FQPhase1&, FQReadLedger1&, uint32_t RecordIndex,
		FQParsedLayout1&, FQFileIdentity1&, FQReadFd1*& OpenFile);
	[[gnu::visibility("hidden")]] VrStatus ObserveOwnedMapping1(FQScratchStorage1&, FQPhase1&, FQReadLedger1&, FQReadFd1& Original,
		const FQFileIdentity1&, const FQParsedLayout1&, uint64_t ActualBias);
	[[gnu::visibility("hidden")]] VrStatus CloseReadFd1(FQReadLedger1&, FQReadFd1&);
	// OPTIONAL real check of one raw full-receipt/corpus size; no eight-size array.
	// Genuine immutable offline FULL-artifact/FULL-receipt qualification still
	// establishes ALL8 extents/types/signatures/imports/closure/RVA32 ends. Runtime
	// full-file SHA + exact8 starts + containing PT_LOAD/native ends/maps binds
	// those facts under actual custody/H/lifetime. Hash/starts alone grant nothing.
	[[gnu::visibility("hidden")]] VrStatus CheckApiFullExtent1(const FQScratchStorage1&, const FQParsedLayout1&,
		uint32_t Ordinal, uint64_t FullReceiptBytes, uint64_t ActualBias);
}
#endif
