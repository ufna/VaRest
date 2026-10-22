// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// Final review I1, proof by shape: compile-time checks from a module outside VaRestHttp, as a game or a custom stage is.
// This file includes only public headers, and this module has no include path into VaRestHttp's Private folder.
// A requires-expression checks access from its own context (the concept's, at namespace scope), so a private member or
// constructor makes it false instead of failing the build; the first check is the control that the idiom sees a public one.

#include "VaRestClient.h"

#include <type_traits>

namespace VaRestTestsSurfaceShapePrivate
{
	template <typename T>
	concept CEditsAttemptHeaders = requires(T& Exchange) { Exchange.GetAttemptHeaders().Set(TEXTVIEW("X-Signed"), TEXTVIEW("1")); };

	template <typename T>
	concept CReachesHandler = requires(T& Exchange) { Exchange.Handler; };

	template <typename T>
	concept CReachesBodyBytes = requires(T& Exchange) { Exchange.Body.Bytes; };

	template <typename T>
	concept CReachesState = requires(T& Exchange) { Exchange.Impl; };

	template <typename T>
	concept CReachesClientState = requires(const T& Client) { Client.GetStateInternal(); };

	template <typename T>
	concept CReachesSpecFields = requires(T& Spec) { Spec.Verb; };

	template <typename T>
	concept CMakesSpec = requires { new T(TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe>(), FStringView(), FStringView()); };

	// Clang only: MSVC's access checking inside requires-expressions is unverified on this project, and a false positive here
	// would break the Windows editor build (wave A concern 1); clang 20 is proven both ways by restore-the-bug (fix-wave-a-report)
#if defined(__clang__)
	static_assert(CEditsAttemptHeaders<FVaRestExchange>, "the control: a stage edits the attempt's headers (the one supported mutation)");
	static_assert(!CReachesHandler<FVaRestExchange>, "FVaRestExchange: no Handler outside VaRestHttp");
	static_assert(!CReachesBodyBytes<FVaRestExchange>, "FVaRestExchange: the body is read through GetBody() only");
	static_assert(!CReachesState<FVaRestExchange>, "FVaRestExchange: its state is private (VaRest::Http::FExchangeAccess only)");
	static_assert(!std::is_constructible_v<TVaRestRequest<FVaRestJson>, TUniquePtr<VaRest::Http::FRequestSpec>&&>,
		"TVaRestRequest: only FVaRestClient makes one from a spec");
	static_assert(std::is_move_constructible_v<TVaRestRequest<FVaRestJson>> && std::is_move_assignable_v<TVaRestRequest<FVaRestJson>>,
		"TVaRestRequest: the builder still moves");
	static_assert(!CReachesClientState<FVaRestClient>, "FVaRestClient: its state is private (VaRest::Http::FClientAccess only)");
	// Bead VaRest-uq3 (plan decision 18): a module cannot hand-build a spec and skip the builder's checks
	static_assert(!CReachesSpecFields<VaRest::Http::FRequestSpec>, "FRequestSpec: its fields are the builder's (bead VaRest-uq3)");
	static_assert(!CMakesSpec<VaRest::Http::FRequestSpec>, "FRequestSpec: only FVaRestClient makes one (bead VaRest-uq3)");
#endif
}
