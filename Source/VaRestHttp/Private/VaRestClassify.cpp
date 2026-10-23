// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestClassify.h"

#include <initializer_list>

namespace VaRest::Http::ClassifyPrivate
{
	bool IsOneOf(FName Detail, std::initializer_list<const TCHAR*> Names)
	{
		for (const TCHAR* Name : Names)
		{
			if (Detail == FName(Name))
			{
				return true;
			}
		}
		return false;
	}
}

EVaRestErrorKind VaRest::Http::KindFor(EVaRestTransportFailure Failure, FName Detail)
{
	using namespace ClassifyPrivate;
	// The request itself (M3 reviews M14, N-4, task 7 M-1, I3; plan decision 15; ruling Q7's misconfigured built-in auth;
	// ruling I-7's refresh refused by the configuration): sending it again fails again
	if (IsOneOf(Detail, { TEXT("BodyRejected"), TEXT("InvalidHeader"), TEXT("UploadReadFailed"), TEXT("BodyFileMissing"),
		TEXT("HttpDisabled"), TEXT("DomainNotAllowed"), TEXT("AuthInvalid"), TEXT("AuthRefreshConfig"), TEXT("PersistenceStopped") }))
	{
		return EVaRestErrorKind::Config;
	}
	// What came back could not be kept (M3 decision 20, review M-7)
	if (IsOneOf(Detail, { TEXT("ResponseFileWriteFailed"), TEXT("ResponseBodyRejected"), TEXT("ResponseTooLarge") }))
	{
		return EVaRestErrorKind::Decode;
	}
	// The auth stages' own failures (Tasks 14-17): a rejected refresh, no credentials, a logout while parked
	if (IsOneOf(Detail, { TEXT("AuthRejected"), TEXT("NoCredentials"), TEXT("SessionCleared") }))
	{
		return EVaRestErrorKind::Auth;
	}
	switch (Failure)
	{
	case EVaRestTransportFailure::None: return EVaRestErrorKind::None;
	case EVaRestTransportFailure::Timeout: return EVaRestErrorKind::Timeout;
	case EVaRestTransportFailure::Canceled: return EVaRestErrorKind::Canceled;
	// Connection, and the engine's Other: on curl a reset or an empty reply (CurlHttp.cpp:1318-1330 makes only five
	// CURLcodes a ConnectionError; HandleRequestFailed calls the rest Other, HttpRequestCommon.cpp:291-310), plan decision 15
	default: return EVaRestErrorKind::Connection;
	}
}

VaRest::Http::FAttemptClass VaRest::Http::ClassifyAttempt(const FVaRestAttemptOutcome& Outcome)
{
	static const FName TooLarge(TEXT("ResponseTooLarge"));
	static const FName Dropped(TEXT("TransportDropped"));
	static const FName PastDeadline(TEXT("Deadline"));
	FAttemptClass Out;
	if (Outcome.IsTransportFailure() && Outcome.FailureDetail == TooLarge && Outcome.Status > 0 && !IsSuccessStatus(Outcome.Status))
	{
		// M3 reviews M-6, M13: an error response whose body passed the JSON cap is still the HTTP error it was
		Out.Kind = EVaRestErrorKind::Http;
		Out.bRetryable = FVaRestError::IsRetryableStatus(Outcome.Status);
		return Out;
	}
	if (Outcome.IsTransportFailure())
	{
		Out.Kind = KindFor(Outcome.Failure, Outcome.FailureDetail);
		// A dropped completion (engine shutdown, NullHttp) would only be dropped again; a deadline will not come back
		Out.bRetryable = (Out.Kind == EVaRestErrorKind::Connection || Out.Kind == EVaRestErrorKind::Timeout)
			&& Outcome.FailureDetail != Dropped && Outcome.FailureDetail != PastDeadline;
		return Out;
	}
	if (!IsSuccessStatus(Outcome.Status))
	{
		Out.Kind = EVaRestErrorKind::Http;
		Out.bRetryable = FVaRestError::IsRetryableStatus(Outcome.Status);
	}
	return Out;
}
