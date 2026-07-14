// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"
#include "HAL/CriticalSection.h"

/**
 * Captures GLog lines on a remote client and stages them for Server RPCs.
 * After a client crash, the host still has everything successfully shipped
 * (typically up to ~1–2 s before death). See docs/crash-repro.md.
 */
class FPFClientLogCapture : public FOutputDevice
{
public:
	explicit FPFClientLogCapture(int32 InMaxBufferChars = 64 * 1024)
		: MaxBufferChars(InMaxBufferChars)
	{
	}

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		// Skip VeryVerbose spam; keep Log/Display/Warning/Error for crash triage.
		if (Verbosity > ELogVerbosity::Log)
		{
			return;
		}
		const double Now = FPlatformTime::Seconds();
		FString Line = FString::Printf(TEXT("[%8.3f][%s][%s] %s\n"),
			Now,
			ToString(Verbosity),
			*Category.ToString(),
			V ? V : TEXT(""));

		FScopeLock Lock(&Mutex);
		Buffer += MoveTemp(Line);
		if (Buffer.Len() > MaxBufferChars)
		{
			// Drop oldest; keep the tail (most recent = most useful for crashes).
			Buffer = Buffer.Right(MaxBufferChars);
		}
	}

	/** Pull up to MaxChars of staged text (empties what was taken). */
	FString TakeChunk(int32 MaxChars)
	{
		FScopeLock Lock(&Mutex);
		if (Buffer.IsEmpty() || MaxChars <= 0)
		{
			return FString();
		}
		const int32 N = FMath::Min(Buffer.Len(), MaxChars);
		FString Out = Buffer.Left(N);
		Buffer.RemoveAt(0, N, EAllowShrinking::No);
		return Out;
	}

	int32 PendingLen() const
	{
		FScopeLock Lock(&Mutex);
		return Buffer.Len();
	}

private:
	mutable FCriticalSection Mutex;
	FString Buffer;
	int32 MaxBufferChars = 64 * 1024;
};
