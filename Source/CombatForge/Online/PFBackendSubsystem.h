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
	FString LanAddr;     // self-reported (in-house only when the row has no distinct public mapping)
	int32   Port = 7777;
	FString Map;
	FString Mode;
	FString Format;
	FString Phase;
	int32   Players = 0;
	int32   MaxPlayers = 12;
	int32   NetProtocol = 0;

	/** "ip:port" ready for `open` — LAN only when both ends are RFC1918 on the same parsed /24
	 *  and the directory row has no distinct public mapping. Matching first-three octets is not LAN. */
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
	/** Weapon unlock slugs from the API (`wpn.ar_m4`, …). Empty offline / until backend seeds rows. */
	TArray<FString> UnlockIds;
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
 *  heartbeats every 10 s (a 409 re-registers — the directory row can expire), and HMAC-signs match
 *  reports (SendMatchReport, called by the GAME MODE at EmitMatchReport when the match enters
 *  Results — not by the rating subsystem). Key sources, first hit wins: -PFServerKey= on the command
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
	/** Active from the LOG IN click (request in flight) until token/cancel/expiry — the UI shows
	 *  CANCEL for this whole window, so a double-click can never start two flows (review 2cf97f2). */
	bool IsDeviceLoginActive() const { return bDeviceCodeRequestInFlight || !PendingDeviceCode.IsEmpty(); }
	const FString& GetPendingUserCode() const { return PendingUserCode; }
	/** Where to approve the code — comes FROM the API response, never hardcoded (2026-07-17: an
	 *  earlier build pointed at a URL that doesn't exist). Empty until BeginDeviceLogin succeeds. */
	const FString& GetVerificationUri() const { return VerificationUri; }

	/** Start the device-link flow. Progress lands on OnStatus; success flips OnAuthChanged. */
	void BeginDeviceLogin();
	void CancelDeviceLogin();
	/** Forget the stored token (local only — no server call needed for v1). */
	void Logout();

	/** Refresh GET /v1/profile/me into GetProfile(). No-op when logged out. */
	void FetchProfile();

	/**
	 * True if the player may equip this weapon. Offline / logged-out / empty unlocks list = fully ungated
	 * (LAN, bots, pre-backend). On fleet, pass WeaponId like "ar_m4" (API stores "wpn.ar_m4").
	 */
	bool IsWeaponUnlocked(const FString& WeaponId) const;
	/** Fleet has loaded unlocks for the local profile (non-empty UnlockIds after profile fetch). */
	bool HasUnlocksLoaded() const { return Profile.UnlockIds.Num() > 0; }

	/** Account rank the unlock gate reads. The alpha-only `pf.SetRank` / DevRankOverride escape
	 *  hatch was removed 2026-07-28 (Tom) ahead of the store beta, so this is now simply the real
	 *  profile level. (P2-ON5) */
	int32 EffectiveRank() const { return Profile.Level; }

	// ---- server directory (player hat; all require login) ----
	void FetchServers(TFunction<void(bool bOk, const TArray<FPFBackendServerInfo>&)> Done);
	void RequestQuickPlay(TFunction<void(bool bOk, const FPFBackendServerInfo&)> Done);
	void RequestJoinByCode(const FString& Code, TFunction<void(bool bOk, const FPFBackendServerInfo&)> Done);

	// ---- fleet hat (dedicated only; inert without a server key) ----
	/** Called by the GameMode once the world is up: registers + starts the heartbeat. */
	void FleetRegisterIfServer(UWorld* World);
	bool IsFleetActive() const { return bFleetRegistered; }
	/** True whenever this box HOLDS a fleet key, registered or not. Match reporting must use this,
	 *  not IsFleetActive(): during a heartbeat-409 re-register (or before the first register lands)
	 *  bFleetRegistered is false, and a match that ended in that window would silently skip the
	 *  fleet XP path and fall through to casual reporting. Queue always, send when able. (P2-ON1) */
	bool HasFleetKey() const { return !ServerKey.IsEmpty(); }
	/** POST /v1/match-report with the HMAC headers. Body is the frozen wire-format JSON the
	 *  GameMode archives locally either way (progression-plan "build now"). A non-empty
	 *  PendingFilePath is deleted on a 200 (the crash-retry queue under Saved/PendingReports/). */
	void SendMatchReport(const FString& ReportJson, const FString& PendingFilePath = FString());

	/**
	 * POST /v1/casual-report — honor-system XP for a LOCAL / listen-hosted / offline match (Tom 2026-07-18).
	 * Authenticated by the PLAYER'S OWN session token (not a server key), so it only ever credits this account;
	 * the Worker halves + daily-caps it and tags every event 'casual_*'. No-op if not logged in. Body carries
	 * only the local player's own stats. Use ONLY when the match is NOT on a fleet server (else XP double-counts).
	 */
	void SendCasualReport(const FString& MatchId, const FString& Mode, const FString& Map, int32 DurationSec,
		int32 Elims, int32 Tags, int32 Objective, int32 Builder, bool bCompleted, bool bWon);

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
	FString VerificationUri;                 // where to approve — FROM the API, never hardcoded
	float   DevicePollIntervalSec = 5.f;
	double  DeviceExpiresAtSec = 0.0;        // FPlatformTime::Seconds deadline
	FTSTicker::FDelegateHandle DevicePollTicker;
	bool    bDeviceCodeRequestInFlight = false;
	/** One /device/token call at a time. On a slow link the fixed poll interval used to stack
	 *  overlapping requests, spamming the API and racing two success handlers. (P2-ON2) */
	bool    bTokenPollInFlight = false;
	/** Bumped by Begin/Cancel/Stop: any async callback carrying a stale generation bails, so a
	 *  canceled flow can never resurrect itself when its in-flight response lands (review 2cf97f2). */
	int32   DeviceFlowGeneration = 0;
	void PollDeviceToken();
	void StopDevicePolling();
	void ArmDevicePollTicker();              // (re)arm at the CURRENT interval (slow_down re-arms)
	void HandleLoginSucceeded(const FString& Token);

	// ---- fleet ----
	bool bFleetRegistered = false;
	bool bFleetRegisterInFlight = false;
	FTSTicker::FDelegateHandle HeartbeatTicker;
	FTSTicker::FDelegateHandle FleetRegisterRetryTicker;
	float FleetRegisterRetryDelaySec = 10.f;   // 10 → 30 → 60, then stay at 60
	int32 PendingReplayInFlight = 0;           // drain latch: skip while prior POSTs are out
	TWeakObjectPtr<ACombatForgeGameState> FleetGS;
	int32 FleetPort = 7777;
	void SendHeartbeat();
	/** Drop out of the directory (POST /unregister + stop heartbeat and register-retry). Safe anytime. */
	void FleetUnregister();
	void ReplayPendingReports();
	void ArmFleetRegisterRetry();

	// ---- plumbing ----
	/** Fire an HTTP request. AuthMode: 0 none, 1 Bearer(player token), 2 Bearer(server key). */
	void Request(const FString& Verb, const FString& Path, const FString& BodyJson, int32 AuthMode,
		TFunction<void(int32 Code, const FString& Body)> Done,
		const TMap<FString, FString>& ExtraHeaders = {});
	void LinkInstallGuid();
	static bool ParseServerInfo(const TSharedPtr<class FJsonObject>& Obj, FPFBackendServerInfo& Out);
};
