// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestClient.h"

namespace VaRest::Http
{
	/**
	 * The one way VaRestHttp's own code reaches FRequestSpec's fields (bead VaRest-uq3). Declared a friend in the public
	 * header and defined only here, in a Private header, so a module outside VaRestHttp cannot use it.
	 */
	struct FRequestSpecAccess
	{
		/** SendRequest's body: the spec into an exchange, the exchange into its client's pipeline (spec 1, 3.1) */
		static FVaRestRequestHandle Send(TUniquePtr<FRequestSpec>&& Spec, const char* SourceFile, uint32 SourceLine);
		/** Final review M7: Kind=Config to the callbacks of a builder made on an empty FVaRestClient */
		static void DeliverWithoutClient(FRequestSpec& Spec);
	};
}
