// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "CombatForgeGameInstance.generated.h"

/**
 * Process-lifetime state: the per-install player identity (T24).
 *
 * A FGuid is generated on first run and persisted at Saved/CombatForge/Identity.json; every
 * later run loads the same GUID. Only the SHA1-hex of the GUID ever goes on the wire or to
 * disk in match records (anonymized voter identity).
 */
UCLASS()
class COMBATFORGE_API UCombatForgeGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	virtual void Init() override;                 // loads/creates identity file

	FGuid   GetLocalPlayerGuid() const;           // per-install identity (T24)
	FString GetLocalPlayerGuidHash() const;       // SHA1-hex of the GUID — what goes on the wire/disk

private:
	// (intra) persistence at Saved/CombatForge/Identity.json
	void LoadOrCreateIdentity();
	FString GetIdentityFilePath() const;

	FGuid   LocalPlayerGuid;
	FString LocalPlayerGuidHash;   // lowercase SHA1-hex, cached at Init
};
