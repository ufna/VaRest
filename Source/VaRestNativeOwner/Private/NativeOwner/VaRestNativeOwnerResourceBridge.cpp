// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestNativeOwnerResourceBridge.h"
#include "VaRestNativeOwnerState.h"

#if VR_NATIVE_OWNER_GNU_X64
// Same-image complete custody bodies. No authority, caller installation,
// resource operation, first-resource binding or second state owner is added.
extern "C"
{
	[[gnu::used, gnu::visibility("hidden")]] VrStatus VR_CALL VrPrivateResourceReserve1(VrOwner* O, VrSession S, const VrResourceSpec1* Spec, uint32_t Bytes, VrResourceReply1* Out)
	{ return VaRestNativeOwnerPrivate::NativeResourceReserve1(O, S, Spec, Bytes, Out); }
	[[gnu::used, gnu::visibility("hidden")]] VrStatus VR_CALL VrPrivateResourceObtained1(VrOwner* O, VrSession S, const VrResourceTicket1* T, const VrResourceActual1* A, uint32_t Bytes, VrResourceReply1* Out)
	{ return VaRestNativeOwnerPrivate::NativeResourceObtained1(O, S, T, A, Bytes, Out); }
	[[gnu::used, gnu::visibility("hidden")]] VrStatus VR_CALL VrPrivateResourceNotObtained1(VrOwner* O, VrSession S, const VrResourceTicket1* T, const VrResourceActual1* A, uint32_t Bytes, VrResourceReply1* Out)
	{ return VaRestNativeOwnerPrivate::NativeResourceNotObtained1(O, S, T, A, Bytes, Out); }
	[[gnu::used, gnu::visibility("hidden")]] VrStatus VR_CALL VrPrivateResourceClosed1(VrOwner* O, VrSession S, const VrResourceTicket1* T, const VrResourceActual1* A, uint32_t Bytes, VrResourceReply1* Out)
	{ return VaRestNativeOwnerPrivate::NativeResourceClosed1(O, S, T, A, Bytes, Out); }
	[[gnu::used, gnu::visibility("hidden")]] VrStatus VR_CALL VrPrivateResourceUncertain1(VrOwner* O, VrSession S, const VrResourceTicket1* T, const VrResourceActual1* A, uint32_t Bytes, VrResourceReply1* Out)
	{ return VaRestNativeOwnerPrivate::NativeResourceUncertain1(O, S, T, A, Bytes, Out); }
}
static_assert(std::is_same_v<decltype(&VrPrivateResourceReserve1), VrPrivateResourceReserve1Fn>);
static_assert(std::is_same_v<decltype(&VrPrivateResourceObtained1), VrPrivateResourceObtained1Fn>);
static_assert(std::is_same_v<decltype(&VrPrivateResourceNotObtained1), VrPrivateResourceNotObtained1Fn>);
static_assert(std::is_same_v<decltype(&VrPrivateResourceClosed1), VrPrivateResourceClosed1Fn>);
static_assert(std::is_same_v<decltype(&VrPrivateResourceUncertain1), VrPrivateResourceUncertain1Fn>);
#endif
