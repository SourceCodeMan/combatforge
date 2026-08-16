// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "Styling/CoreStyle.h"

FORCEINLINE FSlateFontInfo PFSlateFont(int32 Size, bool bBold)
{
	return FCoreStyle::GetDefaultFontStyle(bBold ? FName(TEXT("Bold")) : FName(TEXT("Regular")), Size);
}
