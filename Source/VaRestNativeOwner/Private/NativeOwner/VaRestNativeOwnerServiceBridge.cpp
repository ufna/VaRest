// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestNativeOwnerServiceBridge.h"
#include "VaRestNativeOwnerState.h"
#include "VaRestNativeOwnerAdmission.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)

// Local C entries forward to the actual same-image admission/service bodies.
// The forwarders add no source authority, state, qualification or ownership.
// Every invocation retains the native body's real role/fact/lifetime premises.
extern "C"
{
	[[gnu::used, gnu::visibility("hidden")]] VrStatus VR_CALL VrPrivateAdmitCurrentControl1(uint32_t OutputBytes, VrBootstrap1* Output)
	{
		return VaRestNativeOwnerPrivate::NativeAdmitCurrentControl1(OutputBytes, Output);
	}

	[[gnu::used, gnu::visibility("hidden")]] VrStatus VR_CALL VrPrivateServiceNeverCreated1(VrOwner* Owner, VrSession Session)
	{
		return VaRestNativeOwnerPrivate::NativeServiceNeverCreated(Owner, Session);
	}

	[[gnu::used, gnu::visibility("hidden")]] VrStatus VR_CALL VrPrivateRegisterCurrentServiceThread1(VrOwner* Owner, VrSession Session)
	{
		return VaRestNativeOwnerPrivate::NativeRegisterCurrentServiceThread(Owner, Session);
	}

	[[gnu::used, gnu::visibility("hidden")]] VrStatus VR_CALL VrPrivateServiceJoinObserved1(VrOwner* Owner, VrSession Session)
	{
		return VaRestNativeOwnerPrivate::NativeServiceJoinObserved(Owner, Session);
	}

	[[gnu::used, gnu::visibility("hidden")]] void VR_CALL VrPrivateRequestStop1(VrOwner* Owner, VrSession Session)
	{
		VaRestNativeOwnerPrivate::NativeRequestStop(Owner, Session);
	}

	[[gnu::used, gnu::visibility("hidden")]] VrStatus VR_CALL VrPrivatePumpRegistered1(VrOwner* Owner, VrSession Session, uint32_t Registered)
	{
		return VaRestNativeOwnerPrivate::NativePumpRegistered(Owner, Session, Registered);
	}

	[[gnu::used, gnu::visibility("hidden")]] VrStatus VR_CALL VrPrivatePumpEnter1(VrOwner* Owner, VrSession Session)
	{
		return VaRestNativeOwnerPrivate::NativePumpEnter(Owner, Session);
	}

	[[gnu::used, gnu::visibility("hidden")]] void VR_CALL VrPrivatePumpLeave1(VrOwner* Owner, VrSession Session)
	{
		// SAME verified pinned Enter invocation; native Leave clears only its charge.
		VaRestNativeOwnerPrivate::NativePumpLeave(Owner, Session);
	}

	[[gnu::used, gnu::visibility("hidden")]] void VR_CALL VrPrivateRequestDetachAfterJoin1(VrOwner* Owner, VrSession Session)
	{
		VaRestNativeOwnerPrivate::NativeRequestDetachAfterJoin(Owner, Session);
	}
}

static_assert(sizeof(VrPrivateAdmitControl1Fn) == 8);
static_assert(std::is_same_v<decltype(&VrPrivateAdmitCurrentControl1), VrPrivateAdmitControl1Fn>);
static_assert(sizeof(VrPrivateSessionStatus1Fn) == 8);
static_assert(sizeof(VrPrivateSessionVoid1Fn) == 8);
static_assert(sizeof(VrPrivatePumpRegistered1Fn) == 8);
static_assert(std::is_same_v<decltype(&VrPrivateServiceNeverCreated1), VrPrivateSessionStatus1Fn>);
static_assert(std::is_same_v<decltype(&VrPrivateRegisterCurrentServiceThread1), VrPrivateSessionStatus1Fn>);
static_assert(std::is_same_v<decltype(&VrPrivateServiceJoinObserved1), VrPrivateSessionStatus1Fn>);
static_assert(std::is_same_v<decltype(&VrPrivateRequestStop1), VrPrivateSessionVoid1Fn>);
static_assert(std::is_same_v<decltype(&VrPrivatePumpRegistered1), VrPrivatePumpRegistered1Fn>);
static_assert(std::is_same_v<decltype(&VrPrivatePumpEnter1), VrPrivateSessionStatus1Fn>);
static_assert(std::is_same_v<decltype(&VrPrivatePumpLeave1), VrPrivateSessionVoid1Fn>);
static_assert(std::is_same_v<decltype(&VrPrivateRequestDetachAfterJoin1), VrPrivateSessionVoid1Fn>);

#endif
