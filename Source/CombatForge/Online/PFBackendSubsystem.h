// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Interfaces/IHttpRequest.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "PFBackendSubsystem.generated.h"

class ACombatForgeGameState;

/** One row of the server browser (mirrors combatforge-api GET /v1/servers). */
struct FPFBackendServerInfo
{
	FString ServerId;
	FString Name;
	FString Addr;        // Worker-observed public IP
	FString LanAddr;     // self-reported (same-LAN hairpin fallback); may be empty
	int32   Port = 7777;
	FString Map;
	FString Mode;
	FString Format;
	FString Phase;
	int32   Players = 0;
	int32   MaxPlayers = 12;
	int32   NetProtocol = 0;

	/** "ip:port" ready for `open` — prefers the LAN address when we appear to share its subnet. */
	FString JoinAddress() const;
};

/** Cached slice of GET /v1/profile/me. */
struct FPFBackendProfile
{
	FString DisplayName;
	bool    bIsChild = false;
	int32   XpTotal = 0;
	int32   Level = 1;
	int32   XpInto = 0;      // progress into the current level
	int32   XpNext = 0;      // cost of the next level (0 at cap)
	int32   Matches = 0;
	int32   Wins = 0;
	int32   Eliminations = 0;
};

DECLARE_MULTICAST_DELEGATE(FPFOnBackendAuthChanged);
DECLARE_MULTICAST_DELEGATE_OneParam(FPFOnBackendStatus, const FString& /*HumanReadableLine*/);

/**
 * Client for combatforge-api (accounts + server directory + progression + fleet reporting).
 * Plain REST over FHttpModule — deliberately no OnlineSubsystem (02 D12 stands; see
 * docs/multiplayer-plan.md). Two hats, both worn by this one subsystem:
 *
 *  PLAYER (any build): RFC 8628 device-link login — BeginDeviceLogin() fetches a 6-char code the
 *  menu shows next to "approve at playcombatforge.com/link", then polls for the session token.
 *  The token lives in Saved/CombatForge/Auth.json and rides every /v1 call as a Bearer header.
 *  NO password ever renders in-game. After login the install GUID is linked (retroactive history).
 *
 *  FLEET (dedicated server with a provisioned key): registers with the directory at boot,
 *  heartbeats every 10 s, and HMAC-signs match reports (SendMatchReport, called by the rating
 *  subsystem at CommitMatchRecord). Key sources, first hit wins: -PFServerKey= on the command
 *  line, then Saved/CombatForge/ServerKey.txt. No key (every player install) = fleet path is
 *  completely inert — a listen host can never grant XP by design (progression-plan §1).
 *
 * Api base: [CombatForge.Backend] ApiBaseUrl in Game.ini, -PFApi= override for local dev.
 */
UCLASS()
class COMBATFORGE_API UPFBackendSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ---- auth state (player hat) ----
	bool IsLoggedIn() const { return !AuthToken.IsEmpty(); }
	const FPFBackendProfile& GetProfile() const { return Profile; }
	bool IsDeviceLoginActive() const { return !PendingDeviceCode.IsEmpty(); }
	const FString& GetPendingUserCode() const { return PendingUserCode; }

	/** Start the device-link flow. Progress lands on OnStatus; success flips OnAuthChanged. */
	void BeginDeviceLogin();
	void CancelDeviceLogin();
	/** Forget the stored token (local only — no server call needed for v1). */
	void Logout();

	/** Refresh GET /v1/profile/me into GetProfile(). No-op when logged out. */
	void FetchProfile();

	// ---- server directory (player hat; all require login) ----
	void FetchServers(TFunction<void(bool bOk, const TArray<FPFBackendServerInfo>&)> Done);
	void RequestQuickPlay(TFunction<void(bool bOk, const FPFBackendServerInfo&)> Done);
	void RequestJoinByCode(const FString& Code, TFunction<void(bool bOk, const FPFBackendServerInfo&)> Done);

	// ---- fleet hat (dedicated only; inert without a server key) ----
	/** Called by the GameMode once the world is up: registers + starts the heartbeat. */
	void FleetRegisterIfServer(UWorld* World);
	bool IsFleetActive() const { return bFleetRegistered; }
	/** POST /v1/match-report with the HMAC headers. Body is the frozen wire-format JSON the
	 *  GameMode archives locally either way (progression-plan "build now"). A non-empty
	 *  PendingFilePath is deleted on a 200 (the crash-retry queue under Saved/PendingReports/). */
	void SendMatchReport(const FString& ReportJson, const FString& PendingFilePath = FString());

	// ---- UI subscription points ----
	FPFOnBackendAuthChanged OnAuthChanged;   // token gained/lost, profile refreshed
	FPFOnBackendStatus      OnStatus;        // human-readable one-liners for the menu status row

private:
	// ---- config + persistence ----
	FString ApiBaseUrl;                      // no trailing slash
	FString AuthToken;                       // Better Auth session token (Bearer)
	FString ServerKey;                       // fleet only; empty on player installs
	FPFBackendProfile Profile;
	FString AuthFilePath() const;
	void LoadAuthFromDisk();
	void SaveAuthToDisk() const;

	// ---- device flow ----
	FString PendingDeviceCode;
	FString PendingUserCode;
	float   DevicePollIntervalSec = 5.f;
	double  DeviceExpiresAtSec = 0.0;        // FPlatformTime::Seconds deadline
	FTSTicker::FDelegateHandle DevicePollTicker;
	void PollDeviceToken();
	void StopDevicePolling();
	void HandleLoginSucceeded(const FString& Token);

	// ---- fleet ----
	bool bFleetRegistered = false;
	FTSTicker::FDelegateHandle HeartbeatTicker;
	TWeakObjectPtr<ACombatForgeGameState> FleetGS;
	int32 FleetPort = 7777;
	void SendHeartbeat();

	// ---- plumbing ----
	/** Fire an HTTP request. AuthMode: 0 none, 1 Bearer(player token), 2 Bearer(server key). */
	void Request(const FString& Verb, const FString& Path, const FString& BodyJson, int32 AuthMode,
		TFunction<void(int32 Code, const FString& Body)> Done,
		const TMap<FString, FString>& ExtraHeaders = {});
	void LinkInstallGuid();
	static bool ParseServerInfo(const TSharedPtr<class FJsonObject>& Obj, FPFBackendServerInfo& Out);
};
