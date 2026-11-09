// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)

#include "VaRestNativeOwnerSourceSupplier.h"
#include "VaRestNativeOwnerCallerEntry.h"
#include "VaRestNativeOwnerDeployment1.h"
#include "VaRestNativeOwnerPump.h"

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <type_traits>

static_assert(sizeof(void*)==8 && sizeof(off_t)==8);
static_assert(__BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__);

namespace VaRest::WebSocket::Lws
{
namespace CallerSupplierReadDetail1
{
    // Independent ordinary WS reader. No producer-private include/import/copy
    // of an actual owner. PublicDeployment1 and private calling/object headers
    // share declarations only; all byte-reader bodies remain ordinary WS code.
    using FQReadFd1=FCallerReadFd1;
    using FQReadLedger1=FCallerReadLedger1;
    enum : uint32_t { FdEmpty=0,FdReserved=1,FdKnown=2,FdClosing=3,
        FdClosed=4,FdUncertain=5,FdCatalogue=1,FdOriginal=2,FdSelfMaps=3 };
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


    static VrStatus CloseReadFd1(FQReadLedger1&,FQReadFd1&);
	namespace
	{
		constexpr uint64_t OriginalLimit = 1073741824;
		constexpr char CataloguePath[] = "/opt/varest/native-owner/profile-1/deployment.q0";
		static void Clear(void* P, uint32_t N)
		{ uint8_t* D = static_cast<uint8_t*>(P); for (uint32_t I = 0; I < N; ++I) D[I] = 0; }
		static bool Equal(const void* P, const void* Q, uint32_t N)
		{ const uint8_t* A = static_cast<const uint8_t*>(P); const uint8_t* B = static_cast<const uint8_t*>(Q); uint8_t X = 0; for (uint32_t I = 0; I < N; ++I) X |= A[I] ^ B[I]; return X == 0; }
		static bool Zero(const void* P, uint32_t N)
		{ const uint8_t* A = static_cast<const uint8_t*>(P); uint8_t X = 0; for (uint32_t I = 0; I < N; ++I) X |= A[I]; return X == 0; }
		static uint32_t U32(const uint8_t* P)
		{ return uint32_t(P[0]) | uint32_t(P[1]) << 8 | uint32_t(P[2]) << 16 | uint32_t(P[3]) << 24; }
		static uint64_t ReadLe64(const uint8_t* P)
		{ return uint64_t(U32(P)) | uint64_t(U32(P + 4)) << 32; }

    // Transient raw read coordinates only, built at the TWO fixed same-image
    // phases below. Caller header is NOT an owner-free classification record;
    // it consumes neither a33rd path cell nor another candidate/extent array.
    struct FQReceiptView1
    {
        const uint8_t* Record;
        const uint8_t* Caller;
        const char* Path;
        uint32_t PathBytes, Reserved;
    };
    static uint64_t ReceiptBytes(const FQReceiptView1& V)
    { return ReadLe64(V.Caller ? V.Caller+112 : V.Record+16); }
    static const uint8_t* ReceiptHash(const FQReceiptView1& V)
    { return V.Caller ? V.Caller+120 : V.Record+56; }
    static const uint8_t* ReceiptBuild(const FQReceiptView1& V)
    { return V.Caller ? V.Caller+152 : V.Record+88; }
    static uint32_t ReceiptBuildBytes(const FQReceiptView1& V)
    { return U32(V.Caller ? V.Caller+184 : V.Record+120); }

		static bool Add(uint64_t A, uint64_t B, uint64_t& R)
		{ if (B > UINT64_MAX - A) return false; R = A + B; return true; }
		static bool Relative(uint64_t Start, uint64_t Bytes, uint64_t& End)
		{ return Bytes != 0 && Start <= UINT32_MAX && Add(Start, Bytes, End) && End <= UINT32_MAX; }
		static bool Native(uint64_t Bias, uint64_t RelativeAddress, uint64_t& Address)
		{ return Add(Bias, RelativeAddress, Address) && Address <= UINTPTR_MAX; }
		static bool RoundEnd(uint64_t X, uint64_t Page, uint64_t& R)
		{ if (Page == 0) return false; const uint64_t Rem = X % Page; return Add(X, Rem ? Page - Rem : 0, R); }
		static const uint8_t* Record(const FQScratchStorage1& S, uint32_t I)
		{ return S.Catalogue + VR_Q_HEADER_BYTES + I * VR_Q_RECORD_BYTES; }
		static uint32_t Count(const FQScratchStorage1& S) { return U32(S.Catalogue + VR_Q_H_COUNT); }
		static uint32_t PhdrCount(const FQParsedLayout1& L) { return L.Counts & 255u; }
		static uint32_t LoadCount(const FQParsedLayout1& L) { return (L.Counts >> 8) & 255u; }
		static bool LayoutShape(const FQScratchStorage1& S, const FQParsedLayout1& L)
		{ return Count(S) >= 1 && Count(S) <= 32 && L.RecordIndex < Count(S) && (L.Counts >> 16) == 0
			&& PhdrCount(L) >= 1 && PhdrCount(L) <= 16 && LoadCount(L) >= 1 && LoadCount(L) <= 8; }

		static bool LedgerShape(const FQReadLedger1& L)
		{
			for (uint32_t I = 0; I < 2; ++I)
			{
				const FQReadFd1& C = L.Cells[I];
				if (C.Custody > FdUncertain || C.AcquisitionOrdinal > L.NextOrdinal) return false;
				if (C.Custody == FdEmpty)
				{ if (C.Fd != -1 || C.NativeError || C.Kind || C.AcquisitionOrdinal) return false; }
				else if (C.Kind < FdCatalogue || C.Kind > FdSelfMaps || C.AcquisitionOrdinal == 0
					|| ((C.Custody == FdKnown || C.Custody == FdClosing || C.Custody == FdUncertain) && C.Fd < 0)) return false;
			}
			return L.Cells[0].AcquisitionOrdinal == 0 || L.Cells[0].AcquisitionOrdinal != L.Cells[1].AcquisitionOrdinal;
		}
		static bool Uncertain(const FQReadLedger1& L)
		{ return L.Cells[0].Custody == FdUncertain || L.Cells[1].Custody == FdUncertain; }
		static bool OwnedCell(const FQReadLedger1& L, const FQReadFd1& C)
		{ return &C == &L.Cells[0] || &C == &L.Cells[1]; }
		static VrStatus OpenRead(FQReadLedger1& L, const char* Path, uint32_t Kind, FQReadFd1*& Out)
		{
			Out = nullptr;
			if (Uncertain(L)) return VR_STATUS_RELEASE_UNCERTAIN;
			if (!LedgerShape(L) || Path == nullptr || Kind < 1 || Kind > 3) return VR_STATUS_INVALID_INPUT;
			// Local cells enforce2. The integrating source must preserve caller1+
			// native2 maximum3 and close catalogue BEFORE another local reader.
			for (const FQReadFd1& C : L.Cells)
				if (C.Custody == FdReserved || C.Custody == FdClosing) return VR_STATUS_BUSY;
			for (const FQReadFd1& C : L.Cells)
				if (C.Custody == FdKnown && (C.Kind == FdCatalogue || Kind == FdCatalogue || C.Kind == Kind)) return VR_STATUS_OLD_OWNER_BUSY;
			FQReadFd1* Cell = nullptr;
			for (FQReadFd1& C : L.Cells) if (C.Custody == FdEmpty || C.Custody == FdClosed) { Cell = &C; break; }
			if (Cell == nullptr) return VR_STATUS_CAPACITY;
			if (L.NextOrdinal == UINT64_MAX) return VR_STATUS_SCAN_LIMIT;
			Cell->Fd = -1; Cell->NativeError = 0; Cell->Kind = Kind;
			Cell->AcquisitionOrdinal = ++L.NextOrdinal; Cell->Custody = FdReserved;
			asm volatile("" : : : "memory"); // Materialize reservation before the OS attempt.
			const int Fd = open(Path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
			if (Fd < 0) { Cell->NativeError = errno; Cell->Custody = FdClosed; return VR_STATUS_PROVENANCE; }
			// Infallible custody FIRST; stat/read/hash/options/logging cannot intervene.
			Cell->Fd = Fd; Cell->Custody = FdKnown;
			asm volatile("" : : : "memory"); // No fallible next step precedes custody.
			Out = Cell;
			return VR_STATUS_OK;
		}
		static VrStatus FinishRead(FQReadLedger1& L, FQReadFd1& C, VrStatus Status)
		{ const VrStatus Closed = CloseReadFd1(L, C); return Closed == VR_STATUS_OK ? Status : Closed; }
		static bool SameStat(const struct stat& A, const struct stat& B)
		{
			return A.st_dev == B.st_dev && A.st_ino == B.st_ino && A.st_size == B.st_size
				&& A.st_mtim.tv_sec == B.st_mtim.tv_sec && A.st_mtim.tv_nsec == B.st_mtim.tv_nsec
				&& A.st_ctim.tv_sec == B.st_ctim.tv_sec && A.st_ctim.tv_nsec == B.st_ctim.tv_nsec;
		}
		static VrStatus Regular(const struct stat& S, uint64_t Limit)
		{
			if(!S_ISREG(S.st_mode)||S.st_size<=0)return VR_STATUS_PROVENANCE;
			if(uint64_t(S.st_size)>Limit)return VR_STATUS_SCAN_LIMIT;
			return S.st_mtim.tv_nsec>=0&&S.st_mtim.tv_nsec<1000000000
				&&S.st_ctim.tv_nsec>=0&&S.st_ctim.tv_nsec<1000000000?VR_STATUS_OK:VR_STATUS_PROVENANCE;
		}
		static bool Snapshot(const struct stat& S, FQFileIdentity1& O)
		{
			static_assert(sizeof(S.st_dev) <= 8 && sizeof(S.st_ino) <= 8 && sizeof(S.st_size) <= 8);
			static_assert(sizeof(S.st_mtim.tv_sec) <= 8 && sizeof(S.st_ctim.tv_sec) <= 8);
			static_assert(std::is_signed_v<decltype(S.st_mtim.tv_sec)> && std::is_signed_v<decltype(S.st_ctim.tv_sec)>);
			const uint64_t Major = major(S.st_dev), Minor = minor(S.st_dev);
			if (Major > UINT32_MAX || Minor > UINT32_MAX || S.st_size < 0) return false;
			O.Major = Major; O.Minor = Minor; O.Inode = S.st_ino; O.Bytes = uint64_t(S.st_size);
			O.ModifiedSeconds = S.st_mtim.tv_sec; O.ChangedSeconds = S.st_ctim.tv_sec;
			O.ModifiedNanos = uint32_t(S.st_mtim.tv_nsec); O.ChangedNanos = uint32_t(S.st_ctim.tv_nsec);
			return true;
		}
		static VrStatus ExactRead(int Fd, void* Out, uint32_t Bytes, uint64_t Offset,
			uint64_t FileBytes, uint32_t& Attempts, uint32_t Limit)
		{
			uint64_t End;
			if (!Add(Offset, Bytes, End) || End > FileBytes || End > INT64_MAX) return VR_STATUS_PROVENANCE;
			uint8_t* P = static_cast<uint8_t*>(Out); uint32_t Done = 0;
			while (Done < Bytes)
			{
				if (Attempts == Limit) return VR_STATUS_SCAN_LIMIT;
				++Attempts;
				const uint32_t Request = Bytes - Done > 4096 ? 4096 : Bytes - Done;
				const ssize_t N = pread(Fd, P + Done, Request, static_cast<off_t>(Offset + Done));
				if (N < 0) { const int E = errno; if (E == EINTR) continue; return VR_STATUS_PROVENANCE; }
				if (N == 0 || uint64_t(N) > Request) return VR_STATUS_PROVENANCE;
				Done += uint32_t(N);
			}
			return VR_STATUS_OK;
		}
		static VrStatus EndOfFile(int Fd, uint64_t Bytes, uint8_t* Io, uint32_t& Attempts, uint32_t Limit)
		{
			// At most ONE overflow/EOF observation, including an EINTR result.
			if (Attempts == Limit || Bytes > INT64_MAX) return VR_STATUS_SCAN_LIMIT;
			++Attempts; const ssize_t N = pread(Fd, Io, 1, static_cast<off_t>(Bytes));
			return N == 0 ? VR_STATUS_OK : (N > 0 ? VR_STATUS_SCAN_LIMIT : VR_STATUS_PROVENANCE);
		}

		static uint32_t Rotate(uint32_t X, uint32_t N) { return (X >> N) | (X << (32 - N)); }
		static void ShaBlock(FQSha2561& H)
		{
			static constexpr uint32_t K[64] = {
				0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
				0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
				0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
				0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
				0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
				0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
				0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
				0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
			uint32_t W[64];
			for (uint32_t I = 0; I < 16; ++I)
			{ const uint8_t* P = H.Block + I * 4; W[I] = uint32_t(P[0]) << 24 | uint32_t(P[1]) << 16 | uint32_t(P[2]) << 8 | P[3]; }
			for (uint32_t I = 16; I < 64; ++I)
			{ const uint32_t A = W[I-15], B = W[I-2]; W[I] = W[I-16] + (Rotate(A,7)^Rotate(A,18)^(A>>3)) + W[I-7] + (Rotate(B,17)^Rotate(B,19)^(B>>10)); }
			uint32_t A=H.State[0],B=H.State[1],C=H.State[2],D=H.State[3],E=H.State[4],F=H.State[5],G=H.State[6],J=H.State[7];
			for (uint32_t I = 0; I < 64; ++I)
			{ const uint32_t T = J + (Rotate(E,6)^Rotate(E,11)^Rotate(E,25)) + ((E&F)^(~E&G)) + K[I] + W[I];
				const uint32_t U = (Rotate(A,2)^Rotate(A,13)^Rotate(A,22)) + ((A&B)^(A&C)^(B&C));
				J=G;G=F;F=E;E=D+T;D=C;C=B;B=A;A=T+U; }
			H.State[0]+=A;H.State[1]+=B;H.State[2]+=C;H.State[3]+=D;H.State[4]+=E;H.State[5]+=F;H.State[6]+=G;H.State[7]+=J;
		}
		static void ShaStart(FQSha2561& H)
		{
			Clear(&H,sizeof(H)); const uint32_t Initial[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
			for (uint32_t I=0;I<8;++I) H.State[I]=Initial[I];
		}
		static bool ShaBytes(FQSha2561& H, const uint8_t* P, uint32_t N)
		{
			if (H.Used >= 64 || H.Reserved || N > UINT64_MAX - H.Bytes) return false;
			H.Bytes += N;
			for (uint32_t I=0;I<N;++I) { H.Block[H.Used++]=P[I]; if (H.Used==64) { ShaBlock(H);H.Used=0; } }
			return true;
		}
		static bool ShaEndMatches(FQSha2561& H, const uint8_t* Digest)
		{
			if (H.Bytes > UINT64_MAX/8 || H.Used>=64 || H.Reserved) return false;
			const uint64_t Bits=H.Bytes*8;H.Block[H.Used++]=0x80;
			if (H.Used>56) { while(H.Used<64)H.Block[H.Used++]=0;ShaBlock(H);H.Used=0; }
			while(H.Used<56)H.Block[H.Used++]=0;
			for(uint32_t I=0;I<8;++I)H.Block[56+I]=uint8_t(Bits>>(56-I*8));
			ShaBlock(H);H.Used=0;uint8_t Mismatch=0;
			for(uint32_t I=0;I<32;++I)Mismatch |= uint8_t(H.State[I/4]>>(24-(I%4)*8))^Digest[I];
			return Mismatch==0;
		}

		static bool RvaPair(const uint8_t* Pair)
		{ uint64_t End;return U32(Pair)!=0 && Relative(U32(Pair),U32(Pair+4),End); }
		static bool BuildIdShape(const uint8_t* Id,uint32_t N)
		{ return N>=1 && N<=32 && Zero(Id+N,32-N); }
		static bool PathShape(const uint8_t* P,uint32_t N)
		{
			if(N<2||N>4096||P[0]!='/'||P[N-1]!=0||!Zero(P+N,4096-N))return false;
			uint32_t Component=1;
			for(uint32_t I=1;I<N-1;++I)
			{
				const uint8_t C=P[I];
				if(!((C>='A'&&C<='Z')||(C>='a'&&C<='z')||(C>='0'&&C<='9')||C=='_'||C=='.'||C=='/'||C=='+'||C=='-'))return false;
				if(C=='/')
				{ const uint32_t L=I-Component;if(L==0||(L==1&&P[Component]=='.')||(L==2&&P[Component]=='.'&&P[Component+1]=='.'))return false;Component=I+1; }
			}
			const uint32_t L=N-1-Component;
			return (N==2)|| (L!=0 && !(L==1&&P[Component]=='.') && !(L==2&&P[Component]=='.'&&P[Component+1]=='.'));
		}
		static int PathCompare(const uint8_t* A,const uint8_t* B)
		{ for(uint32_t I=0;I<4096;++I){if(A[I]!=B[I])return A[I]<B[I]?-1:1;if(A[I]==0)return 0;}return 0; }
		static VrStatus RecordSyntax(const uint8_t* R,uint32_t Index,uint32_t& Mask)
		{
			Mask=0;
			if(U32(R+4)!=Index||U32(R+8)!=VR_Q_TARGET||U32(R+12)!=2||U32(R+440)!=3)return VR_STATUS_ABI_INCOMPATIBLE;
			if(ReadLe64(R+16)>OriginalLimit)return VR_STATUS_SCAN_LIMIT;
			if(ReadLe64(R+16)==0||Zero(R+56,32)||Zero(R+368,32)||Zero(R+400,32)
				||!BuildIdShape(R+88,U32(R+120))||!Zero(R+320,48)||U32(R+444))return VR_STATUS_PROVENANCE;
			if(U32(R)==VR_Q_CLASS_OWNER_FREE)
				return Zero(R+24,32)&&Zero(R+124,196)&&U32(R+436)==0 ? VR_STATUS_OK:VR_STATUS_PROVENANCE;
			if(U32(R)!=VR_Q_CLASS_LINEAGE||U32(R+124)!=1||U32(R+128)!=1||U32(R+132)!=1||U32(R+436)!=1)return VR_STATUS_ABI_INCOMPATIBLE;
			if(Zero(R+24,32)||U32(R+164)!=160)return VR_STATUS_PROVENANCE;
			for(uint32_t I=0;I<5;++I)if(!RvaPair(R+136+8*I))return VR_STATUS_PROVENANCE;
			for(uint32_t I=0;I<14;++I)
			{
				const uint8_t* P=R+176+8*I;const uint32_t Start=U32(P),Size=U32(P+4);
				if(Start==0&&Size==0)continue;
				if(!RvaPair(P)||Start==U32(R+136))return VR_STATUS_PROVENANCE;
				for(uint32_t J=0;J<I;++J)if(Start==U32(R+176+8*J))return VR_STATUS_PROVENANCE;
				Mask|=1u<<I;
			}
			if(Mask!=VR_Q_MASK_8&&Mask!=VR_Q_MASK_9&&Mask!=VR_Q_MASK_13&&Mask!=VR_Q_MASK_14)return VR_STATUS_ABI_INCOMPATIBLE;
			// Known72-byte historical objects have NO adjacent-tail premise,
			// including historical14. Only full14 may describe the new216 object.
			const uint32_t ApiBytes=U32(R+172);
			if(ApiBytes!=VR_Q_API_PREFIX_BYTES
				&&(ApiBytes!=VR_Q_API_PRIVATE_OBJECT_BYTES||Mask!=VR_Q_MASK_14))return VR_STATUS_ABI_INCOMPATIBLE;
			for(uint32_t I=0;I<8;++I)
			{
				const uint32_t Start=U32(R+288+4*I);if(Start==0)return VR_STATUS_PROVENANCE;
				for(uint32_t J=0;J<I;++J)if(Start==U32(R+288+4*J))return VR_STATUS_PROVENANCE;
			}
			return VR_STATUS_OK;
		}
		static VrStatus HeaderSyntax(const uint8_t* H,uint64_t ActualBytes,uint32_t& N,uint32_t& Selected)
		{
			N=U32(H+20);Selected=U32(H+96);
			if(!Equal(H,"VRQ0CAT1",8)||U32(H+8)!=VR_Q_FORMAT||U32(H+12)!=256||U32(H+16)!=448||U32(H+60)!=2
				||U32(H+100)!=1||U32(H+104)!=VR_Q_MIN_READER||U32(H+108)!=84||U32(H+188)!=VR_Q_TARGET)return VR_STATUS_ABI_INCOMPATIBLE;
			if(N>32||ReadLe64(H+112)>OriginalLimit)return VR_STATUS_SCAN_LIMIT;
			if(N==0||Selected>=N||U32(H+24)!=4096||U32(H+28)||ReadLe64(H+32)!=ActualBytes
				||ActualBytes!=256+uint64_t(N)*448+uint64_t(N)*4096||ReadLe64(H+40)!=256||ReadLe64(H+48)!=256+uint64_t(N)*448
				||U32(H+56)!=N||Zero(H+64,32)||ReadLe64(H+112)==0||Zero(H+120,32)
				||!BuildIdShape(H+152,U32(H+184))||Zero(H+192,32)||Zero(H+224,32))return VR_STATUS_PROVENANCE;
			return VR_STATUS_OK;
		}
	}

	static VrStatus CloseReadFd1(FQReadLedger1& L,FQReadFd1& C)
	{
		if(!OwnedCell(L,C)||!LedgerShape(L))return Uncertain(L)?VR_STATUS_RELEASE_UNCERTAIN:VR_STATUS_INVALID_INPUT;
		if(C.Custody!=FdKnown)return Uncertain(L)?VR_STATUS_RELEASE_UNCERTAIN:VR_STATUS_STALE;
		if(C.Kind==FdOriginal)
			for(const FQReadFd1& Other:L.Cells)
				if(Other.Kind==FdSelfMaps&&(Other.Custody==FdKnown||Other.Custody==FdReserved||Other.Custody==FdClosing))return Uncertain(L)?VR_STATUS_RELEASE_UNCERTAIN:VR_STATUS_BUSY;
		C.Custody=FdClosing;
		asm volatile("" : : : "memory"); // Sole-close intent precedes the actual call.
		const int R=close(C.Fd);
		if(R!=0){C.NativeError=errno;C.Custody=FdUncertain;return VR_STATUS_RELEASE_UNCERTAIN;}
		C.NativeError=0;C.Custody=FdClosed;
		return Uncertain(L)?VR_STATUS_RELEASE_UNCERTAIN:VR_STATUS_OK;
	}

	static VrStatus ReadCatalogue1(FQScratchStorage1& S,FQPhase1& P,FQReadLedger1& L,uint32_t& OutCount,uint32_t& OutSelected)
	{
		OutCount=0;OutSelected=0;FQReadFd1* F=nullptr;
		VrStatus Status=OpenRead(L,CataloguePath,FdCatalogue,F);if(Status!=VR_STATUS_OK)return Status;
		if(fstat(F->Fd,&P.Before)!=0)return FinishRead(L,*F,VR_STATUS_PROVENANCE);
		Status=Regular(P.Before,VR_Q_FILE_BYTES);if(Status!=VR_STATUS_OK)return FinishRead(L,*F,Status);
		if(P.Before.st_size<256)return FinishRead(L,*F,VR_STATUS_PROVENANCE);
		P.Attempts=0;Clear(S.Catalogue,sizeof(S.Catalogue));Clear(S.Paths,sizeof(S.Paths));
		Status=ExactRead(F->Fd,S.Catalogue,256,0,uint64_t(P.Before.st_size),P.Attempts,128);
		uint32_t N=0,Selected=0;
		if(Status==VR_STATUS_OK)Status=HeaderSyntax(S.Catalogue,uint64_t(P.Before.st_size),N,Selected);
		if(Status==VR_STATUS_OK)Status=ExactRead(F->Fd,S.Catalogue+256,N*448,256,uint64_t(P.Before.st_size),P.Attempts,128);
		if(Status==VR_STATUS_OK)Status=ExactRead(F->Fd,S.Paths,N*4096,256+uint64_t(N)*448,uint64_t(P.Before.st_size),P.Attempts,128);
		if(Status==VR_STATUS_OK)Status=EndOfFile(F->Fd,uint64_t(P.Before.st_size),S.Io,P.Attempts,128);
		if(Status==VR_STATUS_OK&&(fstat(F->Fd,&P.After)!=0||!SameStat(P.Before,P.After)))Status=VR_STATUS_PROVENANCE;
		for(uint32_t I=0;Status==VR_STATUS_OK&&I<N;++I)
		{
			const uint8_t* R=Record(S,I);uint32_t Mask=0;Status=RecordSyntax(R,I,Mask);
			if(Status!=VR_STATUS_OK)break;
			if(!PathShape(S.Paths+I*4096,U32(R+432))||(I&&PathCompare(S.Paths+(I-1)*4096,S.Paths+I*4096)>=0))Status=VR_STATUS_PROVENANCE;
			for(uint32_t J=0;Status==VR_STATUS_OK&&J<I;++J)
			{
				const uint8_t* Q=Record(S,J);
				if(Equal(R+56,Q+56,32)||(U32(R+120)==U32(Q+120)&&Equal(R+88,Q+88,32))
					||(U32(R)==2&&U32(Q)==2&&Equal(R+24,Q+24,32)))Status=VR_STATUS_PROVENANCE;
			}
			if(I==Selected&&(U32(R)!=2||Mask!=VR_Q_MASK_14||U32(R+172)!=VR_Q_API_PRIVATE_OBJECT_BYTES))Status=VR_STATUS_ABI_INCOMPATIBLE;
		}
		Status=FinishRead(L,*F,Status);
		if(Status==VR_STATUS_OK){OutCount=N;OutSelected=Selected;}
		return Status;
	}

	namespace
	{
		static bool Contained(const FQParsedLayout1& L,uint64_t Start,uint64_t Bytes,bool FileBacked,uint32_t Required,uint32_t Forbidden)
		{
			uint64_t End;if(!Relative(Start,Bytes,End))return false;
			for(uint32_t I=0;I<PhdrCount(L);++I)
			{
				const uint8_t* H=L.Phdrs[I];if(U32(H)!=1)continue;
				const uint64_t V=ReadLe64(H+16),N=ReadLe64(H+(FileBacked?32:40));uint64_t E;
				if(!Add(V,N,E))return false;
				if(Start>=V&&End<=E&&(U32(H+4)&Required)==Required&&(U32(H+4)&Forbidden)==0)return true;
			}
			return false;
		}
		static bool NativeExtent(uint64_t Bias,uint64_t Start,uint64_t Bytes)
		{ uint64_t End,A,B;return Relative(Start,Bytes,End)&&Native(Bias,Start,A)&&Native(Bias,End,B); }
		// Original geometry only. Actual initialized nonwrite protection and
		// full source/relocation/custody/loader lifetime remain separate premises.
		// Historical72 is never treated as storage for this larger object.
		static bool CurrentApiOriginalRange(const FQParsedLayout1& L,uint64_t Start,uint64_t Bytes)
		{
			uint64_t End;if(Bytes!=VR_Q_API_PRIVATE_OBJECT_BYTES||!Relative(Start,Bytes,End))return false;
			for(uint32_t I=0;I<PhdrCount(L);++I)
			{
				const uint8_t* H=L.Phdrs[I];if(U32(H)!=1)continue;
				const uint64_t V=ReadLe64(H+16);uint64_t FileEnd;
				if(!Add(V,ReadLe64(H+32),FileEnd))return false;
				if(Start<V||End>FileEnd||(U32(H+4)&4)==0)continue;
				if((U32(H+4)&2)==0)return true;
				// PF_W needs a WHOLE-object original GNU_RELRO span, not merely
				// a protected prefix. The maps pass still requires every byte R/!W.
				for(uint32_t J=0;J<PhdrCount(L);++J)
				{
					const uint8_t* Q=L.Phdrs[J];if(U32(Q)!=0x6474e552u)continue;
					uint64_t RelroEnd;
					if(Relative(ReadLe64(Q+16),ReadLe64(Q+40),RelroEnd)&&Start>=ReadLe64(Q+16)&&End<=RelroEnd)return true;
				}
				return false;
			}
			return false;
		}
		static VrStatus CheckRepresentedRanges(const FQReceiptView1& V,const FQParsedLayout1& L)
		{
            if(V.Caller)return VR_STATUS_OK; // current consumer, never owner-free
            const uint8_t* R=V.Record;
			if(U32(R)==1)return VR_STATUS_OK; // no callable/storage fields in owner-free records
			if(!Contained(L,U32(R+136),U32(R+140),true,5,0))return VR_STATUS_PROVENANCE;
			for(uint32_t I=1;I<=3;++I)
				if(!Contained(L,U32(R+136+I*8),U32(R+140+I*8),false,6,0))return VR_STATUS_PROVENANCE;
			// The existing size pair bounds the WHOLE actual object:72 for
			// history,216 for current. No byte of a tail is read by this reader.
			if(!Contained(L,U32(R+168),U32(R+172),true,4,0))return VR_STATUS_PROVENANCE;
			if(U32(R+172)==VR_Q_API_PRIVATE_OBJECT_BYTES
				&&!CurrentApiOriginalRange(L,U32(R+168),U32(R+172)))return VR_STATUS_PROVENANCE;
			for(uint32_t I=0;I<14;++I)
			{
				const uint8_t* E=R+176+I*8;
				if(U32(E)&&!Contained(L,U32(E),U32(E+4),true,5,0))return VR_STATUS_PROVENANCE;
			}
			for(uint32_t I=0;I<8;++I)
				if(!Contained(L,U32(R+288+I*4),1,true,5,0))return VR_STATUS_PROVENANCE;
			// Starts alone are PARTIAL facts. Genuine immutable offline FULL
			// artifact/receipt qualification supplies ALL8 full extent/type/closure
			// facts; full runtime SHA/starts/PT_LOAD/native ends/maps binds them
			// under actual custody/H/lifetime. The raw scalar checker is optional.
			return VR_STATUS_OK;
		}
		static VrStatus ReadElfLayout(int Fd,FQPhase1& P,FQParsedLayout1& L,const FQReceiptView1& V)
		{
			const uint64_t Bytes=ReceiptBytes(V);
			VrStatus Status=ExactRead(Fd,P.ElfHeader,64,0,Bytes,P.Attempts,VR_Q_ORIGINAL_READS);
			if(Status!=VR_STATUS_OK)return Status;
			const uint8_t* E=P.ElfHeader;
			if(!Equal(E,"\177ELF",4)||E[4]!=2||E[5]!=1||E[6]!=1||(E[7]!=0&&E[7]!=3)||!Zero(E+8,8)
				||E[16]!=3||E[17]!=0||E[18]!=62||E[19]!=0||U32(E+20)!=1
				||E[52]!=64||E[53]!=0||E[54]!=56||E[55]!=0)return VR_STATUS_ABI_INCOMPATIBLE;
			const uint32_t N=uint32_t(E[56])|uint32_t(E[57])<<8;
			const uint64_t Offset=ReadLe64(E+32);uint64_t TableEnd;
			if(N==0||N>16)return VR_STATUS_SCAN_LIMIT;
			if(!Add(Offset,uint64_t(N)*56,TableEnd)||TableEnd>Bytes||TableEnd>UINT32_MAX)return VR_STATUS_PROVENANCE;
			// Bound the optional section-table geometry without parsing symbols or
			// allocating a section table. Extended numbering is unsupported.
			const uint64_t SectionOffset=ReadLe64(E+40);const uint32_t SectionSize=uint32_t(E[58])|uint32_t(E[59])<<8;
			const uint32_t Sections=uint32_t(E[60])|uint32_t(E[61])<<8,Names=uint32_t(E[62])|uint32_t(E[63])<<8;
			uint64_t SectionEnd;
			if(SectionOffset==0){if(Sections||Names)return VR_STATUS_ABI_INCOMPATIBLE;}
			else if(Sections==0||SectionSize!=64||Names>=Sections||!Add(SectionOffset,uint64_t(Sections)*64,SectionEnd)||SectionEnd>Bytes)return VR_STATUS_ABI_INCOMPATIBLE;
			if(U32(E+48)!=0)return VR_STATUS_ABI_INCOMPATIBLE;
			// File offsets stay64-bit. e_phoff is used later for public-PHDR
			// association by the separate caller, not derived from pointer guesses.
			Status=ExactRead(Fd,L.Phdrs,N*56,Offset,Bytes,P.Attempts,VR_Q_ORIGINAL_READS);
			if(Status!=VR_STATUS_OK)return Status;
			const long ActualPage=sysconf(_SC_PAGESIZE);
			if(ActualPage<=0)return VR_STATUS_NATIVE_ERROR;
			P.PageBytes=uint64_t(ActualPage);
			uint32_t Loads=0;bool First=false;P.NoteBytes=0;
			for(uint32_t I=0;I<N;++I)
			{
				const uint8_t* H=L.Phdrs[I];const uint32_t Type=U32(H),Flags=U32(H+4);
				const uint64_t File=ReadLe64(H+8),VirtualAddress=ReadLe64(H+16),FileN=ReadLe64(H+32),MemN=ReadLe64(H+40),Align=ReadLe64(H+48);
				uint64_t FileEnd,MemEnd;
				if(FileN>MemN||!Add(File,FileN,FileEnd)||FileEnd>Bytes
					||(MemN&&!Relative(VirtualAddress,MemN,MemEnd))||(Align>1&&((Align&(Align-1))||File%Align!=VirtualAddress%Align)))return VR_STATUS_ABI_INCOMPATIBLE;
				if(Type==4)
				{
					if(FileN>VR_Q_NOTE_SCAN_BYTES-P.NoteBytes)return VR_STATUS_SCAN_LIMIT;
					P.NoteBytes+=uint32_t(FileN);
				}
				if(Type!=1)continue;
				if(MemN==0){if(FileN)return VR_STATUS_PROVENANCE;continue;}
				if(Loads==8)return VR_STATUS_SCAN_LIMIT;
				if(FileN==0||Flags&~7u||File%P.PageBytes!=VirtualAddress%P.PageBytes)return VR_STATUS_ABI_INCOMPATIBLE;
				uint64_t End,FileVaEnd,FileRounded;
				if(!Add(VirtualAddress,MemN,End)||!RoundEnd(End,P.PageBytes,End)||End>UINT32_MAX
					||!Add(VirtualAddress,FileN,FileVaEnd)||!RoundEnd(FileVaEnd,P.PageBytes,FileRounded)||FileRounded>UINT32_MAX)return VR_STATUS_ABI_INCOMPATIBLE;
				FQLoad1& Load=L.Loads[Loads];Load.Start=VirtualAddress-VirtualAddress%P.PageBytes;Load.End=End;
				Load.FileOffset=File-File%P.PageBytes;Load.Flags=Flags;Load.Reserved=0;
				for(uint32_t J=0;J<Loads;++J)
					if(Load.Start<L.Loads[J].End&&L.Loads[J].Start<Load.End)return VR_STATUS_ABI_INCOMPATIBLE;
				if(Load.Start==0)
				{
					if(First||File!=0||VirtualAddress!=0||!(Flags&4)||FileN<64||TableEnd>FileN)return VR_STATUS_ABI_INCOMPATIBLE;
					First=true;
				}
				++Loads;
			}
			if(!First||Loads==0)return VR_STATUS_ABI_INCOMPATIBLE;
			L.Counts=N|(Loads<<8);
			// Validate every PT_NOTE's whole file-backed mapped range, not its
			// section name. Actual relocation/permission/retention proof is later.
			for(uint32_t I=0;I<N;++I)
			{
				const uint8_t* H=L.Phdrs[I];if(U32(H)==4&&ReadLe64(H+32))
				{
					if(!Contained(L,ReadLe64(H+16),ReadLe64(H+32),true,4,2))return VR_STATUS_PROVENANCE;
					bool Associated=false;
					for(uint32_t J=0;J<N;++J)
					{
						const uint8_t* Load=L.Phdrs[J];uint64_t VEnd,NoteEnd,ExpectedOffset;
						if(U32(Load)!=1||!Add(ReadLe64(Load+16),ReadLe64(Load+32),VEnd)||!Add(ReadLe64(H+16),ReadLe64(H+32),NoteEnd))continue;
						if(ReadLe64(H+16)>=ReadLe64(Load+16)&&NoteEnd<=VEnd&&Add(ReadLe64(Load+8),ReadLe64(H+16)-ReadLe64(Load+16),ExpectedOffset)&&ExpectedOffset==ReadLe64(H+8))Associated=true;
					}
					if(!Associated)return VR_STATUS_PROVENANCE;
				}
			}
			if(ReadLe64(E+24)!=0&&!Contained(L,ReadLe64(E+24),1,true,5,0))return VR_STATUS_ABI_INCOMPATIBLE;
			return CheckRepresentedRanges(V,L);
		}
		static VrStatus ReadElfNotes(int Fd,FQScratchStorage1& S,FQPhase1& P,const FQParsedLayout1& L,const FQReceiptView1& V)
		{
			P.FamilyNotes=0;P.BuildNotes=0;
			for(uint32_t I=0;I<PhdrCount(L);++I)
			{
				const uint8_t* H=L.Phdrs[I];if(U32(H)!=4)continue;
				const uint64_t Base=ReadLe64(H+8),N=ReadLe64(H+32);uint64_t At=0;
				while(At<N)
				{
					if(N-At<12)return VR_STATUS_PROVENANCE;
					VrStatus Status=ExactRead(Fd,S.Io,12,Base+At,ReceiptBytes(V),P.Attempts,VR_Q_ORIGINAL_READS);
					if(Status!=VR_STATUS_OK)return Status;
					const uint32_t Names=U32(S.Io),Desc=U32(S.Io+4),Type=U32(S.Io+8);uint64_t NamePad,DescPad,Total;
					if(!RoundEnd(Names,4,NamePad)||!RoundEnd(Desc,4,DescPad)||!Add(12,NamePad,Total)||!Add(Total,DescPad,Total)||Total>N-At)return VR_STATUS_PROVENANCE;
					const uint32_t Probe=Names>8?8:Names;
					if(Probe)
					{
						Status=ExactRead(Fd,S.Io+12,Probe,Base+At+12,ReceiptBytes(V),P.Attempts,VR_Q_ORIGINAL_READS);
						if(Status!=VR_STATUS_OK)return Status;
					}
					const bool Family=Names>=6&&Equal(S.Io+12,"VaRest",6);
					const bool Gnu=Names==4&&Equal(S.Io+12,"GNU\0",4);
					if(Family)
					{
						if(Names!=7||Type!=0x56524f31||Desc!=64||Total!=84||++P.FamilyNotes!=1||V.Caller||U32(V.Record)!=2)return VR_STATUS_PROVENANCE;
						Status=ExactRead(Fd,S.Io,84,Base+At,ReceiptBytes(V),P.Attempts,VR_Q_ORIGINAL_READS);
						if(Status!=VR_STATUS_OK)return Status;
						if(!Equal(S.Io+12,"VaRest\0",7)||S.Io[19]||!Equal(S.Io+20,"VROwner1",8)
							||U32(S.Io+28)!=1||U32(S.Io+32)!=1||U32(S.Io+36)!=1||U32(S.Io+40)!=1
							||!Equal(S.Io+44,V.Record+24,32)||!Zero(S.Io+76,8))return VR_STATUS_ABI_INCOMPATIBLE;
					}
					else if(Gnu&&Type==3)
					{
						if(Desc==0||Desc>32)return VR_STATUS_SCAN_LIMIT;
						Status=ExactRead(Fd,S.Io,uint32_t(Total),Base+At,ReceiptBytes(V),P.Attempts,VR_Q_ORIGINAL_READS);
						if(Status!=VR_STATUS_OK)return Status;
						if(++P.BuildNotes!=1||Desc!=ReceiptBuildBytes(V)||!Equal(S.Io+16,ReceiptBuild(V),Desc))return VR_STATUS_PROVENANCE;
					}
					At+=Total;
				}
			}
			return P.BuildNotes==1&&(V.Caller?P.FamilyNotes==0:(U32(V.Record)==1?P.FamilyNotes==0:P.FamilyNotes==1))?VR_STATUS_OK:VR_STATUS_PROVENANCE;
		}
	}


    // Original-file consistency ONLY. Genuine selected full-source Profile2
    // receipt qualification remains independently mandatory before opening E.
    // No mapped tail, new loader observation, fd, scratch or read-attempt budget.
    static VrStatus ReadSelectedOriginalHeader2(int Fd, FQScratchStorage1& S, FQPhase1& P,
        const FQParsedLayout1& L, const FQReceiptView1& V)
    {
        if (V.Caller) return VR_STATUS_OK; // Caller WS receipt has no owner API tail.
        const uint8_t* R = V.Record;
        if (!R || U32(R) != 2 || U32(R + 172) != 216) return VR_STATUS_ABI_INCOMPATIBLE;
        const uint64_t Api = U32(R + 168);
        uint64_t ApiEnd, Header, HeaderEnd;
        if (!Add(Api, 216, ApiEnd) || !Add(Api, 72, Header) || !Add(Header, 32, HeaderEnd)
            || HeaderEnd > ApiEnd) return VR_STATUS_PROVENANCE;
        uint32_t Matches = 0; uint64_t Offset = 0;
        for (uint32_t I = 0; I < PhdrCount(L); ++I)
        {
            const uint8_t* H = L.Phdrs[I]; if (U32(H) != 1) continue;
            const uint64_t Start = ReadLe64(H + 16), FileBytes = ReadLe64(H + 32);
            uint64_t End;
            if (!Add(Start, FileBytes, End)) return VR_STATUS_PROVENANCE;
            if (Api < Start || ApiEnd > End) continue;
            uint64_t Candidate, FileEnd;
            if (!Add(ReadLe64(H + 8), Header - Start, Candidate) || !Add(Candidate, 32, FileEnd)
                || FileEnd > ReceiptBytes(V) || FileEnd > INT64_MAX) return VR_STATUS_PROVENANCE;
            ++Matches; Offset = Candidate;
        }
        if (Matches != 1) return VR_STATUS_PROVENANCE;
        // EVERY actual pread, including partial/EINTR, uses SAME P.Attempts.
        const VrStatus Read = ExactRead(Fd, S.Io, 32, Offset, ReceiptBytes(V), P.Attempts, VR_Q_ORIGINAL_READS);
        if (Read != VR_STATUS_OK) return Read;
        if (!Equal(S.Io, "VRPRIV1\0", 8) || U32(S.Io + 8) != 144 || U32(S.Io + 12) != 1
            || U32(S.Io + 16) != 1 || U32(S.Io + 20) != (uint32_t(0x3fff) << 16 | 14u)
            || U32(S.Io + 24) != 2 || U32(S.Io + 28) != 140) return VR_STATUS_ABI_INCOMPATIBLE;
        return VR_STATUS_OK;
    }

    static VrStatus ReadOriginal1(FQScratchStorage1& S,FQPhase1& P,FQReadLedger1& Ledger,
        const FQReceiptView1& V,uint32_t Index,FQParsedLayout1& Layout,
        FQFileIdentity1& Identity,FQReadFd1*& OpenFile)
    {
        OpenFile=nullptr;Clear(&Identity,sizeof(Identity));
        if(Uncertain(Ledger))return VR_STATUS_RELEASE_UNCERTAIN;
        if(Count(S)==0||Count(S)>32||Index>=Count(S)||&Layout!=&S.Candidates[Index]
            ||V.Reserved||V.Path==nullptr||V.PathBytes<2||V.PathBytes>4096
            ||(V.Caller ? (V.Caller!=S.Catalogue||V.Record!=nullptr)
                : (V.Record!=Record(S,Index)||V.Caller!=nullptr)))return VR_STATUS_INVALID_INPUT;
        uint32_t Mask=0;VrStatus Status=VR_STATUS_OK;
        if(!V.Caller)Status=RecordSyntax(V.Record,Index,Mask);
        if(Status!=VR_STATUS_OK)return Status;
        Clear(&Layout,sizeof(Layout));Layout.RecordIndex=Index;
        FQReadFd1* F=nullptr;Status=OpenRead(Ledger,V.Path,FdOriginal,F);
        if(Status!=VR_STATUS_OK)return Status;
        if(fstat(F->Fd,&P.Before)!=0)return FinishRead(Ledger,*F,VR_STATUS_PROVENANCE);
        Status=Regular(P.Before,OriginalLimit);if(Status!=VR_STATUS_OK)return FinishRead(Ledger,*F,Status);
        if(uint64_t(P.Before.st_size)!=ReceiptBytes(V))return FinishRead(Ledger,*F,VR_STATUS_PROVENANCE);
        P.Attempts=0;P.FileOffset=0;ShaStart(P.Sha);
        while(Status==VR_STATUS_OK&&P.FileOffset<ReceiptBytes(V))
        {
            const uint32_t Bytes=uint32_t(ReceiptBytes(V)-P.FileOffset>4096?4096:ReceiptBytes(V)-P.FileOffset);
            Status=ExactRead(F->Fd,S.Io,Bytes,P.FileOffset,ReceiptBytes(V),P.Attempts,VR_Q_ORIGINAL_READS);
            if(Status==VR_STATUS_OK&&!ShaBytes(P.Sha,S.Io,Bytes))Status=VR_STATUS_SCAN_LIMIT;
            P.FileOffset+=Bytes;
        }
        if(Status==VR_STATUS_OK&&!ShaEndMatches(P.Sha,ReceiptHash(V)))Status=VR_STATUS_PROVENANCE;
        if(Status==VR_STATUS_OK)Status=EndOfFile(F->Fd,ReceiptBytes(V),S.Io,P.Attempts,VR_Q_ORIGINAL_READS);
        if(Status==VR_STATUS_OK)Status=ReadElfLayout(F->Fd,P,Layout,V);
        if(Status==VR_STATUS_OK)Status=ReadElfNotes(F->Fd,S,P,Layout,V);
        if(Status==VR_STATUS_OK)Status=ReadSelectedOriginalHeader2(F->Fd,S,P,Layout,V);
        if(Status==VR_STATUS_OK&&(fstat(F->Fd,&P.After)!=0||!SameStat(P.Before,P.After)||!Snapshot(P.After,Identity)))Status=VR_STATUS_PROVENANCE;
        if(Status!=VR_STATUS_OK)return FinishRead(Ledger,*F,Status);
        OpenFile=F;return VR_STATUS_OK; // no implicit/destructor close
    }

	[[gnu::used]] static VrStatus CheckApiFullExtent1(const FQScratchStorage1& S,const FQParsedLayout1& Layout,
		uint32_t Ordinal,uint64_t FullReceiptBytes,uint64_t ActualBias)
	{
		if(!LayoutShape(S,Layout)||Ordinal>=8||FullReceiptBytes==0)return VR_STATUS_INVALID_INPUT;
		const uint8_t* R=Record(S,Layout.RecordIndex);
		if(U32(R)!=2)return VR_STATUS_ABI_INCOMPATIBLE;
		const uint64_t Start=U32(R+288+Ordinal*4);
		if(Start==0||!NativeExtent(ActualBias,Start,FullReceiptBytes))return VR_STATUS_ABI_INCOMPATIBLE;
		return Contained(Layout,Start,FullReceiptBytes,true,5,0)?VR_STATUS_OK:VR_STATUS_PROVENANCE;
	}

	namespace
	{
		static const uint8_t* LoadHeader(const FQParsedLayout1& L,uint32_t Index)
		{
			uint32_t At=0;
			for(uint32_t I=0;I<PhdrCount(L);++I)
				if(U32(L.Phdrs[I])==1&&ReadLe64(L.Phdrs[I]+40)){if(At==Index)return L.Phdrs[I];++At;}
			return nullptr;
		}
		static bool Intersects(uint64_t A,uint64_t B,uint64_t C,uint64_t D)
		{ return A<D&&C<B; }
		static bool RuntimeRangePermissions(const FQMapParser1& M,uint64_t Bias,uint64_t Rva,uint64_t Bytes,uint32_t Need,uint32_t Forbid)
		{
			uint64_t End,A,B;
			if(!Relative(Rva,Bytes,End)||!Native(Bias,Rva,A)||!Native(Bias,End,B))return false;
			return !Intersects(M.Start,M.End,A,B)||((M.Permissions&Need)==Need&&(M.Permissions&Forbid)==0);
		}
		static bool SameFileRowCovered(const FQMapParser1& M,const FQParsedLayout1& L,uint64_t Bias,uint64_t Page)
		{
			// Account for the WHOLE row, not just its intersections with loads.
			// The qualified rounded load ranges are disjoint; at most8 adjacent
			// file-page intervals can advance this cursor. No array or widening.
			uint64_t Cursor=M.Start;
			for(uint32_t Step=0;Step<8&&Cursor<M.End;++Step)
			{
				bool Advanced=false;
				for(uint32_t I=0;I<LoadCount(L);++I)
				{
					const uint8_t* H=LoadHeader(L,I);uint64_t Start,FileEnd,RelativeEnd;
					if(H==nullptr||!Add(ReadLe64(H+16),ReadLe64(H+32),RelativeEnd)
						||!RoundEnd(RelativeEnd,Page,RelativeEnd)||RelativeEnd>UINT32_MAX
						||!Native(Bias,L.Loads[I].Start,Start)||!Native(Bias,RelativeEnd,FileEnd))return false;
					if(Cursor>=Start&&Cursor<FileEnd)
					{
						Cursor=M.End<FileEnd?M.End:FileEnd;Advanced=true;break;
					}
				}
				if(!Advanced)return false; // Outside prefix, suffix or gap.
			}
			return Cursor==M.End;
		}
		static VrStatus CheckRow(FQScratchStorage1& S,FQPhase1& P,const FQFileIdentity1& File,const FQParsedLayout1& L,uint64_t Bias,const FQReceiptView1& V)
		{
			FQMapParser1& M=P.Map;
            (void)S;
			if(M.Start>=M.End||M.Start<M.PreviousEnd||M.Start%P.PageBytes||M.End%P.PageBytes||M.Offset%P.PageBytes)return VR_STATUS_PROVENANCE;
			if(M.Major==File.Major&&M.Minor==File.Minor&&M.Inode==File.Inode
				&&!SameFileRowCovered(M,L,Bias,P.PageBytes))return VR_STATUS_PROVENANCE;
			const uint8_t* R=V.Record;
			bool Touched=false;
			for(uint32_t I=0;I<LoadCount(L);++I)
			{
				const FQLoad1& Load=L.Loads[I];const uint8_t* H=LoadHeader(L,I);if(H==nullptr)return VR_STATUS_PROVENANCE;
				uint64_t Start,End,FileEnd,RelativeFileEnd,FileRoundedEnd,FileOffsetEnd;
				if(!Native(Bias,Load.Start,Start)||!Native(Bias,Load.End,End)
					||!Add(ReadLe64(H+16),ReadLe64(H+32),RelativeFileEnd)||!RoundEnd(RelativeFileEnd,P.PageBytes,RelativeFileEnd)
					||RelativeFileEnd>UINT32_MAX||!Native(Bias,RelativeFileEnd,FileEnd)
					||!RoundEnd(File.Bytes,P.PageBytes,FileRoundedEnd)
					||!Add(Load.FileOffset,RelativeFileEnd-Load.Start,FileOffsetEnd)||FileOffsetEnd>FileRoundedEnd)return VR_STATUS_PROVENANCE;
				if(!Intersects(M.Start,M.End,Start,End))continue;
				Touched=true;
				const uint64_t A=M.Start>Start?M.Start:Start,B=M.End<End?M.End:End;
				if(P.Coverage[I]!=A||(M.Permissions&8)==0)return VR_STATUS_PROVENANCE;
				const uint32_t Expected=((Load.Flags&4)?1u:0u)|((Load.Flags&2)?2u:0u)|((Load.Flags&1)?4u:0u);
				if((M.Permissions&7u)&~Expected)return VR_STATUS_PROVENANCE;
				if((Expected&1)&&!(M.Permissions&1))return VR_STATUS_PROVENANCE;
				if((Expected&4)&&!(M.Permissions&4))return VR_STATUS_PROVENANCE;
				if(A<FileEnd)
				{
					uint64_t RowOffset,LoadOffset;
					if(B>FileEnd||M.Major!=File.Major||M.Minor!=File.Minor||M.Inode!=File.Inode
						||!Add(M.Offset,A-M.Start,RowOffset)||!Add(Load.FileOffset,A-Start,LoadOffset)||RowOffset!=LoadOffset
						||!M.HasPath||M.PathMismatch||M.PathBytes!=V.PathBytes-1)return VR_STATUS_PROVENANCE;
				}
				else
				{
					// Only true trailing p_memsz BSS can use anonymous pages. Never
					// replace a required file page or infer BSS from a missing VMA.
					if(M.Major||M.Minor||M.Inode||M.HasPath)return VR_STATUS_PROVENANCE;
				}
				P.Coverage[I]=B;
			}
			if(!Touched)return VR_STATUS_OK;
			if(!V.Caller&&U32(R)==2)
			{
				if(!RuntimeRangePermissions(M,Bias,U32(R+136),U32(R+140),5,0))return VR_STATUS_PROVENANCE;
				for(uint32_t I=1;I<=3;++I)
					if(!RuntimeRangePermissions(M,Bias,U32(R+136+I*8),U32(R+140+I*8),3,0))return VR_STATUS_PROVENANCE;
				if(!RuntimeRangePermissions(M,Bias,U32(R+168),U32(R+172),1,2))return VR_STATUS_PROVENANCE;
				for(uint32_t I=0;I<14;++I)
					if(U32(R+176+I*8)&&!RuntimeRangePermissions(M,Bias,U32(R+176+I*8),U32(R+180+I*8),5,0))return VR_STATUS_PROVENANCE;
			}
			for(uint32_t I=0;I<PhdrCount(L);++I)
			{
				const uint8_t* H=L.Phdrs[I];
				if(U32(H)==4&&ReadLe64(H+32)&&!RuntimeRangePermissions(M,Bias,ReadLe64(H+16),ReadLe64(H+32),1,2))return VR_STATUS_PROVENANCE;
			}
			return VR_STATUS_OK;
		}
		static int Hex(uint8_t C)
		{ return C>='0'&&C<='9'?C-'0':C>='a'&&C<='f'?C-'a'+10:C>='A'&&C<='F'?C-'A'+10:-1; }
		static bool Digit(FQMapParser1& M,uint8_t C,uint32_t Base,uint32_t MaxDigits,uint64_t Limit)
		{
			const int D=Base==16?Hex(C):(C>='0'&&C<='9'?C-'0':-1);
			if(D<0||uint32_t(D)>=Base||M.Digits==MaxDigits||uint64_t(D)>Limit||M.Number>(Limit-uint32_t(D))/Base)return false;
			M.Number=M.Number*Base+uint32_t(D);++M.Digits;return true;
		}
		static void NextNumber(FQMapParser1& M,uint32_t State)
		{ M.Number=0;M.Digits=0;M.State=State; }
		static VrStatus MapByte(FQScratchStorage1& S,FQPhase1& P,const FQFileIdentity1& File,const FQParsedLayout1& L,uint64_t Bias,const FQReceiptView1& V,uint8_t C)
		{
			FQMapParser1& M=P.Map;
			if(M.Lines==65536||M.LineBytes==4608)return VR_STATUS_SCAN_LIMIT;
			++M.LineBytes;
			if(C==0||C=='\r'||C>126||(C<32&&C!='\n'))return VR_STATUS_PROVENANCE;
			if(C=='\n')
			{
				if(M.State==13&&M.Digits){M.Inode=M.Number;M.State=14;}
				if(M.State!=14&&M.State!=15)return VR_STATUS_PROVENANCE;
				const VrStatus Status=CheckRow(S,P,File,L,Bias,V);if(Status!=VR_STATUS_OK)return Status;
				const uint64_t Previous=M.End;const uint32_t Lines=M.Lines+1;
				Clear(&M,sizeof(M));M.PreviousEnd=Previous;M.Lines=Lines;return VR_STATUS_OK;
			}
			switch(M.State)
			{
			case 0:
				if(C=='-'&&M.Digits){M.Start=M.Number;NextNumber(M,1);break;}
				if(!Digit(M,C,16,16,UINT64_MAX))return VR_STATUS_PROVENANCE;break;
			case 1:
				if(C==' '&&M.Digits){M.End=M.Number;M.State=2;break;}
				if(!Digit(M,C,16,16,UINT64_MAX))return VR_STATUS_PROVENANCE;break;
			case 2:
				if(C==' ')break;if(C!='r'&&C!='-')return VR_STATUS_PROVENANCE;M.Permissions=C=='r'?1u:0u;M.State=3;break;
			case 3:
				if(C!='w'&&C!='-')return VR_STATUS_PROVENANCE;if(C=='w')M.Permissions|=2;M.State=4;break;
			case 4:
				if(C!='x'&&C!='-')return VR_STATUS_PROVENANCE;if(C=='x')M.Permissions|=4;M.State=5;break;
			case 5:
				if(C!='p'&&C!='s')return VR_STATUS_PROVENANCE;if(C=='p')M.Permissions|=8;M.State=6;break;
			case 6:
				if(C!=' ')return VR_STATUS_PROVENANCE;NextNumber(M,7);break;
			case 7:
				if(C==' ')break;M.State=8;if(!Digit(M,C,16,16,UINT64_MAX))return VR_STATUS_PROVENANCE;break;
			case 8:
				if(C==' '&&M.Digits){M.Offset=M.Number;NextNumber(M,9);break;}
				if(!Digit(M,C,16,16,UINT64_MAX))return VR_STATUS_PROVENANCE;break;
			case 9:
				if(C==' ')break;M.State=10;if(!Digit(M,C,16,8,UINT32_MAX))return VR_STATUS_PROVENANCE;break;
			case 10:
				if(C==':'&&M.Digits){M.Major=uint32_t(M.Number);NextNumber(M,11);break;}
				if(!Digit(M,C,16,8,UINT32_MAX))return VR_STATUS_PROVENANCE;break;
			case 11:
				if(C==' '&&M.Digits){M.Minor=uint32_t(M.Number);NextNumber(M,12);break;}
				if(!Digit(M,C,16,8,UINT32_MAX))return VR_STATUS_PROVENANCE;break;
			case 12:
				if(C==' ')break;M.State=13;if(!Digit(M,C,10,20,UINT64_MAX))return VR_STATUS_PROVENANCE;break;
			case 13:
				if(C==' '&&M.Digits){M.Inode=M.Number;M.State=14;break;}
				if(!Digit(M,C,10,20,UINT64_MAX))return VR_STATUS_PROVENANCE;break;
			case 14:
				if(C==' ')break;M.State=15;M.HasPath=1;
				[[fallthrough]];
			case 15:
			{
				const uint32_t Expected=V.PathBytes-1;
				if(M.PathBytes>=Expected||C!=static_cast<uint8_t>(V.Path[M.PathBytes]))M.PathMismatch=1;
				++M.PathBytes;break;
			}
			default:return VR_STATUS_PROVENANCE;
			}
			return VR_STATUS_OK;
		}
	}

	static VrStatus ObserveOwnedMapping1(FQScratchStorage1& S,FQPhase1& P,FQReadLedger1& Ledger,
		FQReadFd1& Original,const FQFileIdentity1& File,const FQParsedLayout1& Layout,uint64_t ActualBias,const FQReceiptView1& V)
	{
		// Same protected local/owned image ONLY. There is no foreign-file open or
		// historical-original claim. Actual source E/O/GT/ActivationDepth and
		// immutable-before-first-load/kernel view/lifetime premises precede this.
		if(Uncertain(Ledger))return VR_STATUS_RELEASE_UNCERTAIN;
		if(!LedgerShape(Ledger)||!OwnedCell(Ledger,Original)||Original.Custody!=FdKnown||Original.Kind!=FdOriginal
			||!LayoutShape(S,Layout)||&Layout!=&S.Candidates[Layout.RecordIndex]||P.PageBytes==0
			||File.Bytes!=ReceiptBytes(V))return VR_STATUS_INVALID_INPUT;
		if(ActualBias%P.PageBytes)return VR_STATUS_ABI_INCOMPATIBLE;
		for(uint32_t I=0;I<LoadCount(Layout);++I)
		{
			uint64_t End;if(!Native(ActualBias,Layout.Loads[I].Start,P.Coverage[I])||!Native(ActualBias,Layout.Loads[I].End,End))return VR_STATUS_ABI_INCOMPATIBLE;
		}
		Clear(&P.Map,sizeof(P.Map));P.PayloadBytes=0;P.Attempts=0;
		FQReadFd1* F=nullptr;VrStatus Status=OpenRead(Ledger,"/proc/self/maps",FdSelfMaps,F);
		if(Status!=VR_STATUS_OK)return Status;
		if(fstat(F->Fd,&P.Before)!=0||fstatfs(F->Fd,&P.FileSystem)!=0||P.FileSystem.f_type!=PROC_SUPER_MAGIC)return FinishRead(Ledger,*F,VR_STATUS_PROVENANCE);
		bool Eof=false;
		while(Status==VR_STATUS_OK&&!Eof)
		{
			if(P.Attempts==8192){Status=VR_STATUS_SCAN_LIMIT;break;}
			const uint64_t Remaining=VR_Q_MAPS_BYTES-P.PayloadBytes;
			const uint32_t Request=Remaining==0?1:uint32_t(Remaining>4096?4096:Remaining);
			++P.Attempts;const ssize_t N=read(F->Fd,S.Io,Request);
			if(N<0){const int E=errno;if(E==EINTR&&Remaining!=0)continue;Status=VR_STATUS_PROVENANCE;break;}
			if(N==0){Eof=true;break;}
			if(uint64_t(N)>Request||Remaining==0){Status=VR_STATUS_SCAN_LIMIT;break;}
			P.PayloadBytes+=uint64_t(N);
			for(uint32_t I=0;Status==VR_STATUS_OK&&I<uint32_t(N);++I)Status=MapByte(S,P,File,Layout,ActualBias,V,S.Io[I]);
		}
		if(Status==VR_STATUS_OK&&(!Eof||P.PayloadBytes==0||P.Map.LineBytes!=0||P.Map.State!=0||P.Map.Lines==0))Status=VR_STATUS_PROVENANCE;
		for(uint32_t I=0;Status==VR_STATUS_OK&&I<LoadCount(Layout);++I)
		{
			uint64_t End;if(!Native(ActualBias,Layout.Loads[I].End,End)||P.Coverage[I]!=End)Status=VR_STATUS_PROVENANCE;
		}
		// Only the maps fd closes here; the original stays genuinely held.
		return FinishRead(Ledger,*F,Status);
	}
    // Public compatibility provenance: accepted62d0b1b5 / authentic public
    // dlfcn.h case contract11. SDK's SAME int dlinfo(void*,int,void*) ABI is used.
    // Actual handler/provider/pointer origin/H remain unproved invocation gates.
    static constexpr int OwnedPhdrRequest1=11;
    struct FQSessionPhase1
    {
        FQFileIdentity1 Identity;
        Dl_info Info;
        FQReadFd1* File;
        FQReceiptView1 View;
        const Elf64_Phdr* ActualPhdrs;
        uint64_t Bias;
        uint32_t Count, Selected;
    };
    static_assert(sizeof(Elf64_Phdr)==56 && sizeof(Lmid_t)==8);
    static_assert(sizeof(FQPhase1)+sizeof(FQSessionPhase1)+2*sizeof(FQReadFd1)<=VR_Q_PHASE_BYTES);
    // Both are ordinary explicit storage declarations; actual simultaneous frame
    // chain512/768/1024/4608 and popping before Admit require fresh machine proof.
    static VrStatus ProtectedPathBytes1(const char* P,uint32_t& Bytes)
    {
        Bytes=0;if(P==nullptr)return VR_STATUS_PROVENANCE;
        constexpr char Prefix[]="/opt/varest/native-owner/profile-1/";
        uint32_t Component=1;
        for(uint32_t I=0;I<4096;++I)
        {
            const uint8_t C=static_cast<uint8_t>(P[I]);
            if(C==0)
            {
                const uint32_t N=I-Component;
                if(I<sizeof(Prefix)||N==0||(N==1&&P[Component]=='.')
                    ||(N==2&&P[Component]=='.'&&P[Component+1]=='.'))return VR_STATUS_PROVENANCE;
                Bytes=I+1;return VR_STATUS_OK;
            }
            if(I<sizeof(Prefix)-1&&C!=static_cast<uint8_t>(Prefix[I]))return VR_STATUS_PROVENANCE;
            if(!((C>='A'&&C<='Z')||(C>='a'&&C<='z')||(C>='0'&&C<='9')
                ||C=='_'||C=='.'||C=='/'||C=='+'||C=='-'))return VR_STATUS_PROVENANCE;
            if(C=='/'&&I!=0)
            {
                const uint32_t N=I-Component;
                if(N==0||(N==1&&P[Component]=='.')||(N==2&&P[Component]=='.'&&P[Component+1]=='.'))return VR_STATUS_PROVENANCE;
                Component=I+1;
            }
        }
        return VR_STATUS_SCAN_LIMIT;
    }
    static bool SameProtectedName1(const char* Actual,const FQReceiptView1& V)
    {
        if(Actual==nullptr||V.PathBytes<2||V.PathBytes>4096)return false;
        for(uint32_t I=0;I<V.PathBytes;++I)
        {
            if(Actual[I]!=V.Path[I])return false;
            if(Actual[I]==0)return I+1==V.PathBytes;
        }
        return false;
    }
    static VrStatus MappedNotes1(FQPhase1& P,const FQParsedLayout1& L,const FQReceiptView1& V,uint64_t Bias)
    {
        P.FamilyNotes=0;P.BuildNotes=0;P.NoteBytes=0;
        for(uint32_t I=0;I<PhdrCount(L);++I)
        {
            const uint8_t* H=L.Phdrs[I];if(U32(H)!=4)continue;
            const uint64_t N=ReadLe64(H+32);uint64_t Base,End;
            if(N>VR_Q_NOTE_SCAN_BYTES-P.NoteBytes)return VR_STATUS_SCAN_LIMIT;
            P.NoteBytes+=uint32_t(N);
            if(N==0)continue;
            if(!Contained(L,ReadLe64(H+16),N,true,4,2)||!Native(Bias,ReadLe64(H+16),Base)
                ||!Add(Base,N,End)||End>UINTPTR_MAX)return VR_STATUS_PROVENANCE;
            uint64_t At=0;
            while(At<N)
            {
                if(N-At<12)return VR_STATUS_PROVENANCE;
                const uint8_t* B=reinterpret_cast<const uint8_t*>(uintptr_t(Base+At));
                const uint32_t Names=U32(B),Desc=U32(B+4),Type=U32(B+8);
                uint64_t NamePad,DescPad,Total;
                if(!RoundEnd(Names,4,NamePad)||!RoundEnd(Desc,4,DescPad)
                    ||!Add(12,NamePad,Total)||!Add(Total,DescPad,Total)||Total>N-At)return VR_STATUS_PROVENANCE;
                const bool Family=Names>=6&&Equal(B+12,"VaRest",6);
                const bool Gnu=Names==4&&Equal(B+12,"GNU\0",4);
                if(Family)
                {
                    if(V.Caller||Names!=7||Desc!=64||Total!=84||Type!=0x56524f31
                        ||++P.FamilyNotes!=1||!Equal(B+12,"VaRest\0",7)||B[19]
                        ||!Equal(B+20,"VROwner1",8)||U32(B+28)!=1||U32(B+32)!=1
                        ||U32(B+36)!=1||U32(B+40)!=1||!Equal(B+44,V.Record+24,32)
                        ||!Zero(B+76,8))return VR_STATUS_PROVENANCE;
                }
                else if(Gnu&&Type==3)
                {
                    if(Desc==0||Desc>32)return VR_STATUS_SCAN_LIMIT;
                    if(++P.BuildNotes!=1||Desc!=ReceiptBuildBytes(V)
                        ||!Equal(B+16,ReceiptBuild(V),Desc))return VR_STATUS_PROVENANCE;
                }
                At+=Total;
            }
        }
        return P.BuildNotes==1&&(V.Caller?P.FamilyNotes==0:P.FamilyNotes==1)
            ?VR_STATUS_OK:VR_STATUS_PROVENANCE;
    }
    static VrStatus ObserveEntryAddress1(FQSessionPhase1& C,const FCallerEJournal1& E,uint64_t Address)
    {
        void* Map=nullptr;C.Info={};
        if(::dladdr1(reinterpret_cast<const void*>(uintptr_t(Address)),&C.Info,&Map,RTLD_DL_LINKMAP)==0
            ||Map==nullptr||Map!=E.OpaqueMap
            ||reinterpret_cast<uintptr_t>(C.Info.dli_fbase)!=C.Bias
            ||!SameProtectedName1(C.Info.dli_fname,C.View))return VR_STATUS_PROVENANCE;
        // Hidden local symbols need not have public nearest-symbol names. Their
        // ordinary full extents/signature/types/closures are genuine offline
        // receipt prerequisites, bound by full file SHA/mapping, not dli_saddr.
        return VR_STATUS_OK;
    }
    static void StorePrivateAddress1(FCallerPrivateBinding1& B,uint32_t I,uint64_t Address)
    {
        switch(I)
        {
        case 0:B.Admit=reinterpret_cast<VrPrivateAdmitControl1Fn>(uintptr_t(Address));break;
        case 1:B.NeverCreated=reinterpret_cast<VrPrivateSessionStatus1Fn>(uintptr_t(Address));break;
        case 2:B.RegisterCurrentThread=reinterpret_cast<VrPrivateSessionStatus1Fn>(uintptr_t(Address));break;
        case 3:B.JoinObserved=reinterpret_cast<VrPrivateSessionStatus1Fn>(uintptr_t(Address));break;
        case 4:B.RequestStop=reinterpret_cast<VrPrivateSessionVoid1Fn>(uintptr_t(Address));break;
        case 5:B.PumpRegistered=reinterpret_cast<VrPrivatePumpRegistered1Fn>(uintptr_t(Address));break;
        case 6:B.PumpEnter=reinterpret_cast<VrPrivateSessionStatus1Fn>(uintptr_t(Address));break;
        case 7:B.PumpLeave=reinterpret_cast<VrPrivateSessionVoid1Fn>(uintptr_t(Address));break;
        case 8:B.RequestDetachAfterJoin=reinterpret_cast<VrPrivateSessionVoid1Fn>(uintptr_t(Address));break;
        case 9:B.Reserve=reinterpret_cast<VrPrivateResourceReserve1Fn>(uintptr_t(Address));break;
        case 10:B.Obtained=reinterpret_cast<VrPrivateResourceObtained1Fn>(uintptr_t(Address));break;
        case 11:B.NotObtained=reinterpret_cast<VrPrivateResourceNotObtained1Fn>(uintptr_t(Address));break;
        case 12:B.Closed=reinterpret_cast<VrPrivateResourceClosed1Fn>(uintptr_t(Address));break;
        case 13:B.Uncertain=reinterpret_cast<VrPrivateResourceUncertain1Fn>(uintptr_t(Address));break;
        }
    }
    static VrStatus QualifySelectedAddresses1(FQScratchStorage1& S,FQSessionPhase1& C,
        const FCallerEJournal1& E,FCallerPrivateBinding1& B)
    {
        const uint8_t* R=C.View.Record;const FQParsedLayout1& L=S.Candidates[C.Selected];
        uint64_t Address;
        if(!NativeExtent(C.Bias,U32(R+136),U32(R+140))
            ||!Contained(L,U32(R+136),U32(R+140),true,5,0)
            ||!Native(C.Bias,U32(R+136),Address))return VR_STATUS_PROVENANCE;
        VrStatus Status=ObserveEntryAddress1(C,E,Address);if(Status!=VR_STATUS_OK)return Status;
        B.Getter=reinterpret_cast<FCallerBootstrapFn1>(uintptr_t(Address));
        for(uint32_t I=0;I<14;++I)
        {
            const uint8_t* Pair=R+176+I*8;
            if(!NativeExtent(C.Bias,U32(Pair),U32(Pair+4))
                ||!Contained(L,U32(Pair),U32(Pair+4),true,5,0)
                ||!Native(C.Bias,U32(Pair),Address))return VR_STATUS_PROVENANCE;
            Status=ObserveEntryAddress1(C,E,Address);if(Status!=VR_STATUS_OK)return Status;
            StorePrivateAddress1(B,I,Address);
        }
        for(uint32_t I=0;I<8;++I)
        {
            // Only the compact start is present. Genuine immutable FULL8 API
            // extents/types/closure/RVA32 ends are the accepted offline premise;
            // full SHA + exact starts + complete PT_LOAD/native ends/maps binds
            // it. No inferred sizes/extra array/reader or qualification Boolean.
            if(!NativeExtent(C.Bias,U32(R+288+4*I),1)
                ||!Contained(L,U32(R+288+4*I),1,true,5,0)
                ||!Native(C.Bias,U32(R+288+4*I),Address))return VR_STATUS_PROVENANCE;
            Status=ObserveEntryAddress1(C,E,Address);if(Status!=VR_STATUS_OK)return Status;
        }
        B.Bias=C.Bias;B.InventorySchema=2;B.PresentMask=VR_Q_MASK_14;
        return VR_STATUS_OK;
    }
    static VrStatus OpenAndObserveEntry1(FQScratchStorage1& S,FQPhase1& P,FQSessionPhase1& C,
        FCallerReadLedger1& Files,FCallerEJournal1& E,FCallerPrivateBinding1& Binding)
    {
        if(E.State!=CallerEEmpty1||E.Handle||E.AcquisitionOrdinal||E.Reserved)return VR_STATUS_PROVENANCE;
        if(Files.NextOrdinal==UINT64_MAX)return VR_STATUS_SCAN_LIMIT;
        if(Uncertain(Files))return VR_STATUS_RELEASE_UNCERTAIN;
        const uint8_t* R=C.View.Record;uint32_t Mask=0;
        VrStatus Status=RecordSyntax(R,C.Selected,Mask);
        if(Status!=VR_STATUS_OK)return Status;
        // Current14/storage216 ONLY, before the ONE actual E open.
        // Historical14/storage72 and8/9/13 never reach E or a tail probe.
        if(U32(R)!=2||Mask!=VR_Q_MASK_14||U32(R+172)!=VR_Q_API_PRIVATE_OBJECT_BYTES)return VR_STATUS_ABI_INCOMPATIBLE;
        E.AcquisitionOrdinal=++Files.NextOrdinal;E.State=CallerEReserved1;
        asm volatile("" : : : "memory");
        void* const Handle=::dlopen(C.View.Path,RTLD_NOW|RTLD_LOCAL);
        if(Handle==nullptr){E.State=CallerEClosed1;return VR_STATUS_NATIVE_ERROR;}
        E.Handle=Handle;E.State=CallerEUnverified1;
        asm volatile("" : : : "memory"); // Actual E credit before ANY query.
        Lmid_t Namespace=LM_ID_BASE;
        if(::dlinfo(Handle,RTLD_DI_LMID,&Namespace)!=0)return VR_STATUS_PROVENANCE;
        E.Namespace=static_cast<int64_t>(Namespace);
        if(Namespace!=LM_ID_BASE)return VR_STATUS_NAMESPACE_INELIGIBLE;
        struct link_map* Opaque=nullptr;
        if(::dlinfo(Handle,RTLD_DI_LINKMAP,&Opaque)!=0||Opaque==nullptr)return VR_STATUS_PROVENANCE;
        E.OpaqueMap=Opaque; // Equality identity ONLY; NO private map field access.
        C.ActualPhdrs=nullptr;
        const int N=::dlinfo(Handle,OwnedPhdrRequest1,&C.ActualPhdrs);
        if(N>16)return VR_STATUS_SCAN_LIMIT;
        if(N<=0||C.ActualPhdrs==nullptr)return VR_STATUS_PROVENANCE;
        const FQParsedLayout1& L=S.Candidates[C.Selected];
        if(uint32_t(N)!=PhdrCount(L))return VR_STATUS_PROVENANCE;
        const uint64_t Address=reinterpret_cast<uintptr_t>(C.ActualPhdrs),Offset=ReadLe64(P.ElfHeader+32);
        uint64_t TableEnd;
        if(Address%alignof(Elf64_Phdr)||Address<Offset||!Add(Address,uint64_t(N)*56,TableEnd)
            ||TableEnd>UINTPTR_MAX)return VR_STATUS_PROVENANCE;
        C.Bias=Address-Offset;
        if(!NativeExtent(C.Bias,Offset,uint64_t(N)*56)
            ||!Equal(C.ActualPhdrs,L.Phdrs,uint32_t(N)*56))return VR_STATUS_PROVENANCE;
        Status=ObserveOwnedMapping1(S,P,Files,*C.File,C.Identity,L,C.Bias,C.View);
        if(Status!=VR_STATUS_OK)return Status;
        // Public PHDR table is readable under held E; actual header/note ranges
        // are now original-fd+map bound. No descriptor/cold API table is read.
        if(!Equal(reinterpret_cast<const void*>(uintptr_t(C.Bias)),P.ElfHeader,64))return VR_STATUS_PROVENANCE;
        Status=MappedNotes1(P,L,C.View,C.Bias);if(Status!=VR_STATUS_OK)return Status;
        Status=QualifySelectedAddresses1(S,C,E,Binding);if(Status!=VR_STATUS_OK)return Status;
        E.State=CallerEVerified1;
        return VR_STATUS_OK;
    }

} // CallerSupplierReadDetail1


[[gnu::used]] VrStatus FNativeSourceSupplier::TryCreateForCurrentControl(FCallerEntryTransaction& T, FService& Service)
{
    // An actual source-owned transaction, not a caller-supplied authority/profile.
    // Run repeats physical-role/fork checks before Gate and its FIRST file open.
    return T.RunForService(Service);
}

[[gnu::used]] VrStatus FNativeSourceSupplier::QualifyOriginalEntryWithScratch1(FCallerEntryTransaction& T)
{
    using namespace CallerSupplierReadDetail1;
    // Gate1 was actually acquired on this initiating stack. No field appoints
    // H/source custody or grants entry; all those premises remain independently
    // closed. This ordinary out-of-line frame MUST pop before native admission.
    if(T.Phase!=1||T.Inspections!=2)return VR_STATUS_INVALID_INPUT;
    FQScratchStorage1 Scratch;
    FQPhase1 Phase{};
    FQSessionPhase1 C{};
    Clear(&T.SelectedEntry,sizeof(T.SelectedEntry));
    VrStatus Status=ReadCatalogue1(Scratch,Phase,T.Files,C.Count,C.Selected);
    if(Status!=VR_STATUS_OK)return Status;
    // One owned WS anchor observation on THIS real substantive locally bound
    // function. No empty anchor stub, WS loader pin or foreign census.
    void* OwnMap=nullptr;
    if(::dladdr1(reinterpret_cast<const void*>(&FNativeSourceSupplier::QualifyOriginalEntryWithScratch1),&C.Info,&OwnMap,RTLD_DL_LINKMAP)==0
        ||OwnMap==nullptr||C.Info.dli_fbase==nullptr)return VR_STATUS_PROVENANCE;
    C.View.Caller=Scratch.Catalogue;C.View.Record=nullptr;C.View.Path=C.Info.dli_fname;
    Status=ProtectedPathBytes1(C.View.Path,C.View.PathBytes);if(Status!=VR_STATUS_OK)return Status;
    C.Bias=reinterpret_cast<uintptr_t>(C.Info.dli_fbase);
    Status=ReadOriginal1(Scratch,Phase,T.Files,C.View,C.Selected,Scratch.Candidates[C.Selected],C.Identity,C.File);
    if(Status!=VR_STATUS_OK)return Status;
    Status=ObserveOwnedMapping1(Scratch,Phase,T.Files,*C.File,C.Identity,Scratch.Candidates[C.Selected],C.Bias,C.View);
    if(Status==VR_STATUS_OK&&!Equal(reinterpret_cast<const void*>(uintptr_t(C.Bias)),Phase.ElfHeader,64))Status=VR_STATUS_PROVENANCE;
    if(Status==VR_STATUS_OK)Status=MappedNotes1(Phase,Scratch.Candidates[C.Selected],C.View,C.Bias);
    const VrStatus OwnClosed=CloseReadFd1(T.Files,*C.File);
    if(OwnClosed!=VR_STATUS_OK)return OwnClosed;
    if(Status!=VR_STATUS_OK)return Status;
    // Reuse ONLY this existing candidate cell after the WS original/maps have
    // known closed. Its current caller receipt remains in the header, not in an
    // invented owner-free record or33rd dictionary/candidate slot.
    C.View.Caller=nullptr;C.View.Record=Record(Scratch,C.Selected);
    C.View.Path=reinterpret_cast<const char*>(Scratch.Paths+C.Selected*4096);
    C.View.PathBytes=U32(C.View.Record+432);
    Status=ReadOriginal1(Scratch,Phase,T.Files,C.View,C.Selected,Scratch.Candidates[C.Selected],C.Identity,C.File);
    if(Status!=VR_STATUS_OK)return Status;
    Status=OpenAndObserveEntry1(Scratch,Phase,C,T.Files,T.Entry,T.SelectedEntry);
    if(Status!=VR_STATUS_OK)Clear(&T.SelectedEntry,sizeof(T.SelectedEntry));
    // Original stays in T.Files through real native return/E disposition. No
    // pointer into Scratch/Phase/C escapes and no native body is called here.
    return Status;
}

[[gnu::used]] VrStatus FNativeSourceSupplier::CloseKnownCallerReadFds1(FCallerReadLedger1& Files)
{
    using namespace CallerSupplierReadDetail1;
    if(!LedgerShape(Files))return VR_STATUS_RELEASE_UNCERTAIN;
    VrStatus Result=Uncertain(Files)?VR_STATUS_RELEASE_UNCERTAIN:VR_STATUS_OK;
    // At most2 represented actual fds. Close maps before original, never retry a
    // historical uncertain fd; another independent KNOWN fd still closes once.
    for(uint32_t Kind=3;Kind!=0;--Kind)
        for(FQReadFd1& F:Files.Cells)
            if(F.Kind==Kind&&F.Custody==FdKnown)
            {
                const VrStatus Closed=CloseReadFd1(Files,F);
                if(Closed!=VR_STATUS_OK)Result=VR_STATUS_RELEASE_UNCERTAIN;
            }
    for(const FQReadFd1& F:Files.Cells)
        if(F.Custody!=FdEmpty&&F.Custody!=FdClosed)Result=VR_STATUS_RELEASE_UNCERTAIN;
    return Result;
}

} // VaRest::WebSocket::Lws
#endif
