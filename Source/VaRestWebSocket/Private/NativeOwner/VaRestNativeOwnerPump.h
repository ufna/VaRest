// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestNativeOwnerHost1.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)

#include "VaRestNativeOwnerPrivateService1.h"

namespace VaRest::WebSocket::Lws
{
	class FNativeOwnerServiceAdapter;

	// Copied calling data ONLY. Shape checks and private constructors do not qualify
	// addresses, authenticate a caller or supply the missing Supplier/admission.
	struct FNativeOwnerServiceCalls1
	{
		VrPrivateSessionStatus1Fn NeverCreated = nullptr;
		VrPrivateSessionStatus1Fn RegisterCurrentThread = nullptr;
		VrPrivateSessionStatus1Fn JoinObserved = nullptr;
		VrPrivateSessionVoid1Fn RequestStop = nullptr;
		VrPrivatePumpRegistered1Fn PumpRegistered = nullptr;
		VrPrivateSessionStatus1Fn PumpEnter = nullptr;
		VrPrivateSessionVoid1Fn PumpLeave = nullptr;
		VrPrivateSessionVoid1Fn RequestDetachAfterJoin = nullptr;
	};

	struct FNativeOwnerServiceValues
	{
		VrImageId Image {};
		VrOwner* Owner = nullptr;
		VrOwnerApi2 Api {};
		FNativeOwnerServiceCalls1 Calls {};
	};

	[[gnu::visibility("hidden")]] bool NativeOwnerValuesHaveValidShape(const FNativeOwnerServiceValues& Values);
	[[gnu::visibility("hidden")]] VrStatus RequireNativeOwnerControlThread(const TCHAR* Operation);
	[[gnu::visibility("hidden")]] bool NativeOwnerRealThreadModeAllowed();

	class [[gnu::visibility("hidden")]] FActiveOwnerPump final : public TSharedFromThis<FActiveOwnerPump, ESPMode::ThreadSafe>
	{
		friend class FNativeOwnerServiceAdapter;

	public:
		// Preconditions: creating owner accounted actual Stop/deregistration and
		// host code lifetime spans any final callback/Leave/return/destructor.
		// Violation is reported; destruction never performs native cleanup or ack.
		~FActiveOwnerPump();

	private:
		static TSharedPtr<FActiveOwnerPump, ESPMode::ThreadSafe> Create(const FNativeOwnerServiceValues&, VrSession);
		FActiveOwnerPump(const FNativeOwnerServiceValues&, VrSession);
		FActiveOwnerPump(const FActiveOwnerPump&) = delete;
		FActiveOwnerPump& operator=(const FActiveOwnerPump&) = delete;
		VrStatus RegisterBeforeServicePermit();
		VrStatus StopOnControlThread();
		bool CanDropServiceReference() const;
		FVaRestNativeOwnerHostStep1 PumpOnControlThread();

		const VrImageId Image;
		VrOwner* const Owner;
		const VrOwnerApi2 Api;
		const VrSession Session;
		const VrPrivateSessionVoid1Fn RequestStop;
		const VrPrivatePumpRegistered1Fn SetRegistered;
		const VrPrivateSessionStatus1Fn Enter;
		const VrPrivateSessionVoid1Fn Leave;
        // Layout carry ONLY: permanently default-invalid, never assigned/reset
        // or used as registration/driver authority. No AddSP/Remove replacement.
		FDelegateHandle BeginFrameHandle;
		bool Enabled = false;
		bool InCallback = false;
		bool Stopped = false;
		bool StopIssued = false;
		bool Registering = false;
		bool NativeRegistrationKnown = false;
		bool RemovalPending = false;
		VrStatus LastRemovalStatus = VR_STATUS_PENDING;
	};
}

#endif
