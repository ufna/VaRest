// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestNumericConnectorNative.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)

#include <type_traits>

namespace VaRest::WebSocket::NumericConnectorC1
{
namespace
{
    static bool ParserSeparate(const void* A, uint64_t N, const void* B, uint64_t M)
    {
        if (N == 0 || M == 0) return true;
        if (A == nullptr || B == nullptr) return true; // Invalid input is handled after valid output framing.
        const uintptr_t X = reinterpret_cast<uintptr_t>(A), Y = reinterpret_cast<uintptr_t>(B);
        if (N > UINTPTR_MAX - X || M > UINTPTR_MAX - Y) return false;
        return X + N <= Y || Y + M <= X;
    }
    static uint8_t Lower(uint8_t C) { return C >= 'A' && C <= 'Z' ? C + ('a' - 'A') : C; }
    static bool Equal(const uint8_t* A, uint32_t N, const char* B, uint32_t M)
    {
        if (N != M) return false;
        for (uint32_t I = 0; I < N; ++I) if (A[I] != static_cast<uint8_t>(B[I])) return false;
        return true;
    }
    static bool Name(const uint8_t* A, uint32_t N, const char* B, uint32_t M)
    {
        if (N != M) return false;
        for (uint32_t I = 0; I < N; ++I) if (Lower(A[I]) != static_cast<uint8_t>(B[I])) return false;
        return true;
    }
    static bool Token(uint8_t C)
    {
        if ((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9')) return true;
        constexpr char Extra[] = "!#$%&'*+-.^_`|~";
        for (uint32_t I = 0; I + 1 < sizeof(Extra); ++I) if (C == static_cast<uint8_t>(Extra[I])) return true;
        return false;
    }
    static EStatus1 Result(FParsedHeader1& Out, EStatus1 Status)
    {
        Out.Outcome = static_cast<uint32_t>(Status);
        if (Status != EStatus1::OK) { Out.HeaderEnd = 0; Out.MatchedSlot = UINT32_MAX; Out.StatusCode = 0; }
        return Status;
    }
    static bool Output(FParsedHeader1* Out)
    {
        if (Out == nullptr || reinterpret_cast<uintptr_t>(Out) % alignof(FParsedHeader1) != 0) return false;
        Out->Outcome = static_cast<uint32_t>(EStatus1::INVALID);
        Out->HeaderEnd = 0; Out->MatchedSlot = UINT32_MAX; Out->StatusCode = 0;
        return true;
    }
    static EStatus1 Frame(const uint8_t* Bytes, uint32_t Length, uint32_t& End)
    {
        End = 0;
        if (Length > HeaderBytes1) return EStatus1::PROTOCOL;
        if (Bytes == nullptr && Length != 0) return EStatus1::INVALID;
        for (uint32_t I = 0; I < Length; ++I)
        {
            const uint8_t C = Bytes[I];
            if (C == '\r')
            {
                if (I + 1 == Length) return Length == HeaderBytes1 ? EStatus1::PROTOCOL : EStatus1::PENDING;
                if (Bytes[I + 1] != '\n') return EStatus1::PROTOCOL;
                if (I >= 2 && Bytes[I - 2] == '\r' && Bytes[I - 1] == '\n') { End = I + 2; return EStatus1::OK; }
                ++I;
            }
            else if (C < 32 || C > 126) return EStatus1::PROTOCOL;
        }
        return Length == HeaderBytes1 ? EStatus1::PROTOCOL : EStatus1::PENDING;
    }
    static uint32_t LineEnd(const uint8_t* Bytes, uint32_t Cursor, uint32_t End)
    {
        while (Cursor < End && Bytes[Cursor] != '\r') ++Cursor;
        return Cursor;
    }
    struct FFields { uint32_t AuthAt, AuthBytes, HostAt, HostBytes, AuthCount, HostCount; };
    static bool Fields(const uint8_t* Bytes, uint32_t Cursor, uint32_t End, FFields& Out)
    {
        uint32_t Count = 0;
        while (Cursor + 2 < End)
        {
            const uint32_t Last = LineEnd(Bytes, Cursor, End);
            if (Last == Cursor || ++Count > 32) return false;
            uint32_t Colon = Cursor;
            while (Colon < Last && Bytes[Colon] != ':')
            {
                if (!Token(Bytes[Colon])) return false;
                ++Colon;
            }
            if (Colon == Cursor || Colon == Last) return false;
            const uint32_t N = Colon - Cursor;
            uint32_t First = Colon + 1, ValueEnd = Last;
            while (First < ValueEnd && Bytes[First] == ' ') ++First;
            while (ValueEnd > First && Bytes[ValueEnd - 1] == ' ') --ValueEnd;
            if (Name(Bytes + Cursor, N, "content-length", 14) || Name(Bytes + Cursor, N, "transfer-encoding", 17)) return false;
            if (Name(Bytes + Cursor, N, "proxy-authorization", 19))
            {
                if (++Out.AuthCount != 1) return false;
                Out.AuthAt = First; Out.AuthBytes = ValueEnd - First;
            }
            if (Name(Bytes + Cursor, N, "host", 4))
            {
                if (++Out.HostCount != 1) return false;
                Out.HostAt = First; Out.HostBytes = ValueEnd - First;
            }
            Cursor = Last + 2;
        }
        return Cursor + 2 == End;
    }
    static bool Expectation(const FIngressExpectation1& E)
    {
        if (E.Authority == nullptr || E.Base64 == nullptr || E.AuthorityBytes > 512 || E.Base64Bytes != 96
            || E.Authority[E.AuthorityBytes] != '\0' || E.Base64[96] != '\0') return false;
        for (uint32_t I = 0; I < E.AuthorityBytes; ++I)
            if (static_cast<uint8_t>(E.Authority[I]) <= 32 || static_cast<uint8_t>(E.Authority[I]) > 126) return false;
        return true;
    }
    static uint32_t CredentialDifference(const uint8_t* Actual, uint32_t N, const char* Expected)
    {
        // Fixed96 iterations for BOTH slots. Length is public; no secret early exit.
        uint32_t Difference = N ^ 96u;
        for (uint32_t I = 0; I < 96; ++I)
        {
            const uint8_t Byte = I < N ? Actual[I] : 0;
            Difference |= Byte ^ static_cast<uint8_t>(Expected[I]);
        }
        return Difference;
    }
}

[[gnu::used, gnu::visibility("hidden")]] EStatus1 ParseIngress1(const uint8_t* Bytes, uint32_t Length,
    const FIngressExpectation1 Expected[2], FParsedHeader1* Out)
{
    if (!ParserSeparate(Bytes, Length, Out, sizeof(*Out)) || !ParserSeparate(Expected, 2 * sizeof(*Expected), Out, sizeof(*Out)))
        return EStatus1::INVALID;
    if (Expected != nullptr && reinterpret_cast<uintptr_t>(Expected) % alignof(FIngressExpectation1) == 0)
    {
        for (uint32_t I = 0; I < 2; ++I)
        {
            // Caller owns the actual accessible spans; only checked bounded geometry is claimed.
            if (Expected[I].AuthorityBytes <= 512 && (!ParserSeparate(Expected[I].Authority, Expected[I].AuthorityBytes + 1, Out, sizeof(*Out))
                || !ParserSeparate(Expected[I].Base64, 97, Out, sizeof(*Out)))) return EStatus1::INVALID;
        }
    }
    if (!Output(Out)) return EStatus1::INVALID;
    if (Expected == nullptr || reinterpret_cast<uintptr_t>(Expected) % alignof(FIngressExpectation1) != 0
        || !Expectation(Expected[0]) || !Expectation(Expected[1])) return Result(*Out, EStatus1::INVALID);
    uint32_t End = 0;
    const EStatus1 Framed = Frame(Bytes, Length, End);
    if (Framed != EStatus1::OK) return Result(*Out, Framed);
    if (End != Length) return Result(*Out, EStatus1::PROTOCOL);
    const uint32_t FirstEnd = LineEnd(Bytes, 0, End);
    if (FirstEnd < 18 || !Equal(Bytes, 8, "CONNECT ", 8)) return Result(*Out, EStatus1::PROTOCOL);
    const uint32_t AuthorityEnd = FirstEnd - 9;
    if ((!Equal(Bytes + AuthorityEnd, 9, " HTTP/1.0", 9) && !Equal(Bytes + AuthorityEnd, 9, " HTTP/1.1", 9))
        || AuthorityEnd <= 8 || AuthorityEnd - 8 > 512) return Result(*Out, EStatus1::PROTOCOL);
    for (uint32_t I = 8; I < AuthorityEnd; ++I) if (Bytes[I] == ' ') return Result(*Out, EStatus1::PROTOCOL);
    FFields F {};
    if (!Fields(Bytes, FirstEnd + 2, End, F) || F.AuthCount != 1 || F.AuthBytes < 6
        || !Name(Bytes + F.AuthAt, 5, "basic", 5) || Bytes[F.AuthAt + 5] != ' '
        || (F.HostCount != 0 && !Equal(Bytes + F.HostAt, F.HostBytes,
            reinterpret_cast<const char*>(Bytes + 8), AuthorityEnd - 8))) return Result(*Out, EStatus1::PROTOCOL);
    const uint8_t* Actual = Bytes + F.AuthAt + 6;
    const uint32_t ActualBytes = F.AuthBytes - 6;
    const uint32_t Difference0 = CredentialDifference(Actual, ActualBytes, Expected[0].Base64);
    const uint32_t Difference1 = CredentialDifference(Actual, ActualBytes, Expected[1].Base64);
    const bool Match0 = Difference0 == 0 && Expected[0].AuthorityBytes != 0
        && Equal(Bytes + 8, AuthorityEnd - 8, Expected[0].Authority, Expected[0].AuthorityBytes);
    const bool Match1 = Difference1 == 0 && Expected[1].AuthorityBytes != 0
        && Equal(Bytes + 8, AuthorityEnd - 8, Expected[1].Authority, Expected[1].AuthorityBytes);
    if (static_cast<uint32_t>(Match0) + static_cast<uint32_t>(Match1) != 1) return Result(*Out, EStatus1::AUTH_DENIED);
    Out->HeaderEnd = End; Out->MatchedSlot = Match0 ? 0u : 1u;
    return Result(*Out, EStatus1::OK);
}

[[gnu::used, gnu::visibility("hidden")]] EStatus1 ParseProxyResponse1(const uint8_t* Bytes, uint32_t Length, FParsedHeader1* Out)
{
    if (!ParserSeparate(Bytes, Length, Out, sizeof(*Out))) return EStatus1::INVALID;
    if (!Output(Out)) return EStatus1::INVALID;
    uint32_t End = 0;
    const EStatus1 Framed = Frame(Bytes, Length, End);
    if (Framed != EStatus1::OK) return Result(*Out, Framed);
    const uint32_t FirstEnd = LineEnd(Bytes, 0, End);
    if (FirstEnd < 12 || (!Equal(Bytes, 9, "HTTP/1.0 ", 9) && !Equal(Bytes, 9, "HTTP/1.1 ", 9)))
        return Result(*Out, EStatus1::PROTOCOL);
    uint32_t Code = 0;
    for (uint32_t I = 9; I < 12; ++I)
    {
        if (Bytes[I] < '0' || Bytes[I] > '9') return Result(*Out, EStatus1::PROTOCOL);
        Code = Code * 10 + Bytes[I] - '0';
    }
    if (Code < 200 || (FirstEnd != 12 && Bytes[12] != ' ')) return Result(*Out, EStatus1::PROTOCOL);
    FFields F {};
    if (!Fields(Bytes, FirstEnd + 2, End, F)) return Result(*Out, EStatus1::PROTOCOL);
    Out->HeaderEnd = End; Out->StatusCode = Code;
    return Result(*Out, EStatus1::OK);
}
static_assert(sizeof(FParsedHeader1) == 16 && alignof(FParsedHeader1) == 4);
static_assert(sizeof(FIngressExpectation1) == 24 && alignof(FIngressExpectation1) == 8);
static_assert(std::is_trivial_v<FParsedHeader1> && std::is_standard_layout_v<FIngressExpectation1>);
}
#endif
