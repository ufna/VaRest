// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonWriter.h"

#include "VaRestJsonLog.h"
#include "VaRestJsonSax.h"

namespace VaRest::Json
{
	bool Write(const FNode& Node, EVaRestJsonFormat Format, TArray<uint8>& Out)
	{
		const int32 Start = Out.Num();
		FUtf8Sink Sink{ Out };
		FWriterAllocator StackAllocator;
		FEmitState State;
		bool bWritten = false;
		if (Format == EVaRestJsonFormat::Pretty)
		{
			FPrettyWriter Writer(Sink, &StackAllocator);
			Writer.SetIndent(' ', 2);
			bWritten = EmitNode(Writer, Node, 0, State);
		}
		else
		{
			FCondensedWriter Writer(Sink, &StackAllocator);
			bWritten = EmitNode(Writer, Node, 0, State);
		}
		if (!bWritten)
		{
			Out.SetNum(Start, EAllowShrinking::No);
			return false;
		}
		if (State.NonFinite > 0)
		{
			UE_LOG(LogVaRestJson, Warning, TEXT("FVaRestJson: %d non-finite number(s) written as null (JSON has no NaN/Infinity)"), State.NonFinite);
		}
		return true;
	}
}
