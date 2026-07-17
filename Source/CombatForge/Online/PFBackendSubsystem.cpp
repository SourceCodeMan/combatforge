// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Online/PFBackendSubsystem.h"

#include "CombatForge.h"
#include "Core/CombatForgeGameInstance.h"
#include "Core/CombatForgeGameState.h"
#include "Core/CombatForgePlayerState.h"

#include "Dom/JsonObject.h"
#include "GameFramework/PlayerState.h"
#include "HAL/FileManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"

// OpenSSL typedefs `UI` (ui_st), which collides with Unreal's UI namespace — same rename
// workaround the engine's SSL module uses.
#define UI OPENSSL_UI
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
#include <openssl/hmac.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace
{
	constexpr float HeartbeatPeriodSec = 10.f;   // directory marks us stale after 30 s (3 missed)
	const TCHAR* DefaultApiBase = TEXT("https://api.playcombatforge.com");
	const TCHAR* GameClientId   = TEXT("combatforge-game");

	FString EnumShortName(const TCHAR* EnumPath, int64 Value)
	{
		const UEnum* Enum = FindObject<UEnum>(nullptr, EnumPath);
		return Enum ? Enum->GetNameStringByValue(Value) : FString::FromInt((int32)Value);
	}

	/** HMAC-SHA256 lowercase hex over "<ts>.<body>" — must match the Worker's verification. */
	FString HmacSha256Hex(const FString& Key, const FString& Message)
	{
		const FTCHARToUTF8 KeyUtf8(*Key);
		const FTCHARToUTF8 MsgUtf8(*Message);
		unsigned char Digest[EVP_MAX_MD_SIZE];
		unsigned int DigestLen = 0;
		HMAC(EVP_sha256(),
			KeyUtf8.Get(), KeyUtf8.Length(),
			reinterpret_cast<const unsigned char*>(MsgUtf8.Get()), MsgUtf8.Length(),
			Digest, &DigestLen);
		FString Hex;
		Hex.Reserve(DigestLen * 2);
		for (unsigned int i = 0; i < DigestLen; ++i)
		{
			Hex += FString::Printf(TEXT("%02x"), Digest[i]);
		}
		return Hex;
	}
}

FString FPFBackendServerInfo::JoinAddress() const
{
	// Same-LAN hairpin fallback: if the directory gave us a LAN address and our own primary
	// adapter sits in the same /24, the public IP would hairpin through the router (flaky on
	// consumer gear) — go direct instead.
	if (!LanAddr.IsEmpty())
	{
		bool bCanBind = false;
		if (ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			const TSharedRef<FInternetAddr> Local = Sockets->GetLocalHostAddr(*GLog, bCanBind);
			FString LocalIp = Local->ToString(/*bAppendPort=*/false);
			int32 LastDotA, LastDotB;
			if (LocalIp.FindLastChar(TEXT('.'), LastDotA) && LanAddr.FindLastChar(TEXT('.'), LastDotB)
				&& LocalIp.Left(LastDotA) == LanAddr.Left(LastDotB))
			{
				return FString::Printf(TEXT("%s:%d"), *LanAddr, Port);
			}
		}
	}
	return FString::Printf(TEXT("%s:%d"), *Addr, Port);
}

// ---------------------------------------------------------------------------
// Lifecycle + config
// ---------------------------------------------------------------------------

void UPFBackendSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Api base: ini default, -PFApi= wins (local wrangler dev: -PFApi=http://127.0.0.1:8787).
	ApiBaseUrl = DefaultApiBase;
	GConfig->GetString(TEXT("CombatForge.Backend"), TEXT("ApiBaseUrl"), ApiBaseUrl, GGameIni);
	FParse::Value(FCommandLine::Get(), TEXT("PFApi="), ApiBaseUrl);
	while (ApiBaseUrl.EndsWith(TEXT("/")))
	{
		ApiBaseUrl.LeftChopInline(1);
	}

	// Fleet key (dedicated boxes only): command line, then Saved/CombatForge/ServerKey.txt.
	FParse::Value(FCommandLine::Get(), TEXT("PFServerKey="), ServerKey);
	if (ServerKey.IsEmpty())
	{
		FString FromDisk;
		if (FFileHelper::LoadFileToString(FromDisk,
			*FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CombatForge"), TEXT("ServerKey.txt"))))
		{
			ServerKey = FromDisk.TrimStartAndEnd();
		}
	}

	LoadAuthFromDisk();
	if (IsLoggedIn())
	{
		FetchProfile();   // token might be stale — a 401 here demotes us to logged-out cleanly
	}
	UE_LOG(CombatForgeLog, Log, TEXT("Backend: api=%s loggedIn=%d fleetKey=%s"),
		*ApiBaseUrl, IsLoggedIn() ? 1 : 0, ServerKey.IsEmpty() ? TEXT("no") : TEXT("YES"));
}

void UPFBackendSubsystem::Deinitialize()
{
	StopDevicePolling();
	if (HeartbeatTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(HeartbeatTicker);
		HeartbeatTicker.Reset();
	}
	Super::Deinitialize();
}

FString UPFBackendSubsystem::AuthFilePath() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CombatForge"), TEXT("Auth.json"));
}

void UPFBackendSubsystem::LoadAuthFromDisk()
{
	FString Contents;
	if (!FFileHelper::LoadFileToString(Contents, *AuthFilePath()))
	{
		return;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Contents);
	if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
	{
		Root->TryGetStringField(TEXT("token"), AuthToken);
		Root->TryGetStringField(TEXT("displayName"), Profile.DisplayName);
	}
}

void UPFBackendSubsystem::SaveAuthToDisk() const
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema"), 1);
	Root->SetStringField(TEXT("token"), AuthToken);
	Root->SetStringField(TEXT("displayName"), Profile.DisplayName);
	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Root, Writer);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(AuthFilePath()), /*Tree=*/true);
	FFileHelper::SaveStringToFile(Output, *AuthFilePath());
}

// ---------------------------------------------------------------------------
// HTTP plumbing
// ---------------------------------------------------------------------------

void UPFBackendSubsystem::Request(const FString& Verb, const FString& Path, const FString& BodyJson,
	int32 AuthMode, TFunction<void(int32, const FString&)> Done,
	const TMap<FString, FString>& ExtraHeaders)
{
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetURL(ApiBaseUrl + Path);
	Req->SetVerb(Verb);
	Req->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	if (AuthMode == 1 && !AuthToken.IsEmpty())
	{
		Req->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AuthToken));
	}
	else if (AuthMode == 2 && !ServerKey.IsEmpty())
	{
		Req->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ServerKey));
	}
	for (const TPair<FString, FString>& H : ExtraHeaders)
	{
		Req->SetHeader(H.Key, H.Value);
	}
	if (!BodyJson.IsEmpty())
	{
		Req->SetContentAsString(BodyJson);
	}
	Req->SetTimeout(15.f);
	Req->OnProcessRequestComplete().BindLambda(
		[Done](FHttpRequestPtr /*R*/, FHttpResponsePtr Resp, bool bOk)
		{
			if (Done)
			{
				Done(bOk && Resp.IsValid() ? Resp->GetResponseCode() : 0,
					bOk && Resp.IsValid() ? Resp->GetContentAsString() : FString());
			}
		});
	Req->ProcessRequest();
}

// ---------------------------------------------------------------------------
// Device-link login (player hat)
// ---------------------------------------------------------------------------

void UPFBackendSubsystem::BeginDeviceLogin()
{
	if (IsDeviceLoginActive())
	{
		return;
	}
	OnStatus.Broadcast(TEXT("Contacting playcombatforge.com…"));
	const FString Body = FString::Printf(TEXT("{\"client_id\":\"%s\"}"), GameClientId);
	TWeakObjectPtr<UPFBackendSubsystem> WeakThis(this);
	Request(TEXT("POST"), TEXT("/api/auth/device/code"), Body, /*AuthMode=*/0,
		[WeakThis](int32 Code, const FString& Resp)
		{
			UPFBackendSubsystem* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp);
			if (Code != 200 || !FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				Self->OnStatus.Broadcast(TEXT("Couldn't reach the account server — try again."));
				return;
			}
			Root->TryGetStringField(TEXT("device_code"), Self->PendingDeviceCode);
			Root->TryGetStringField(TEXT("user_code"), Self->PendingUserCode);
			double Interval = 5.0, ExpiresIn = 900.0;
			Root->TryGetNumberField(TEXT("interval"), Interval);
			Root->TryGetNumberField(TEXT("expires_in"), ExpiresIn);
			Self->DevicePollIntervalSec = FMath::Clamp((float)Interval, 2.f, 30.f);
			Self->DeviceExpiresAtSec = FPlatformTime::Seconds() + ExpiresIn;
			Self->OnStatus.Broadcast(FString::Printf(
				TEXT("Code %s — approve at playcombatforge.com/link"), *Self->PendingUserCode));

			Self->DevicePollTicker = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([WeakThis](float) -> bool
				{
					if (UPFBackendSubsystem* Inner = WeakThis.Get())
					{
						Inner->PollDeviceToken();
						return true;   // keep ticking; StopDevicePolling removes us
					}
					return false;
				}), Self->DevicePollIntervalSec);
		});
}

void UPFBackendSubsystem::CancelDeviceLogin()
{
	StopDevicePolling();
	OnStatus.Broadcast(TEXT("Log-in canceled."));
}

void UPFBackendSubsystem::PollDeviceToken()
{
	if (PendingDeviceCode.IsEmpty())
	{
		return;
	}
	if (FPlatformTime::Seconds() > DeviceExpiresAtSec)
	{
		StopDevicePolling();
		OnStatus.Broadcast(TEXT("Code expired — press LOG IN for a fresh one."));
		return;
	}
	const FString Body = FString::Printf(
		TEXT("{\"grant_type\":\"urn:ietf:params:oauth:grant-type:device_code\",")
		TEXT("\"device_code\":\"%s\",\"client_id\":\"%s\"}"), *PendingDeviceCode, GameClientId);
	TWeakObjectPtr<UPFBackendSubsystem> WeakThis(this);
	Request(TEXT("POST"), TEXT("/api/auth/device/token"), Body, /*AuthMode=*/0,
		[WeakThis](int32 Code, const FString& Resp)
		{
			UPFBackendSubsystem* Self = WeakThis.Get();
			if (!Self || Self->PendingDeviceCode.IsEmpty())
			{
				return;
			}
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp);
			if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				return;   // transient — next poll retries
			}
			FString Token;
			if (Code == 200 && (Root->TryGetStringField(TEXT("access_token"), Token)
				|| Root->TryGetStringField(TEXT("token"), Token)))
			{
				Self->HandleLoginSucceeded(Token);
				return;
			}
			FString Err;
			Root->TryGetStringField(TEXT("error"), Err);
			if (Err == TEXT("authorization_pending"))
			{
				return;   // player hasn't approved yet — keep polling
			}
			if (Err == TEXT("slow_down"))
			{
				Self->DevicePollIntervalSec += 5.f;   // RFC 8628 §3.5 (ticker keeps old cadence
				return;                                // until re-armed; server tolerates it)
			}
			Self->StopDevicePolling();
			Self->OnStatus.Broadcast(Err == TEXT("access_denied")
				? TEXT("Link was denied on the website.")
				: TEXT("Log-in failed — press LOG IN to retry."));
		});
}

void UPFBackendSubsystem::StopDevicePolling()
{
	if (DevicePollTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DevicePollTicker);
		DevicePollTicker.Reset();
	}
	PendingDeviceCode.Reset();
	PendingUserCode.Reset();
}

void UPFBackendSubsystem::HandleLoginSucceeded(const FString& Token)
{
	AuthToken = Token;
	StopDevicePolling();
	SaveAuthToDisk();
	OnStatus.Broadcast(TEXT("Signed in!"));
	OnAuthChanged.Broadcast();
	FetchProfile();
	LinkInstallGuid();
}

void UPFBackendSubsystem::Logout()
{
	AuthToken.Reset();
	Profile = FPFBackendProfile();
	IFileManager::Get().Delete(*AuthFilePath());
	OnStatus.Broadcast(TEXT("Signed out."));
	OnAuthChanged.Broadcast();
}

void UPFBackendSubsystem::FetchProfile()
{
	if (!IsLoggedIn())
	{
		return;
	}
	TWeakObjectPtr<UPFBackendSubsystem> WeakThis(this);
	Request(TEXT("GET"), TEXT("/v1/profile/me"), FString(), /*AuthMode=*/1,
		[WeakThis](int32 Code, const FString& Resp)
		{
			UPFBackendSubsystem* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}
			if (Code == 401)
			{
				// Stored token no longer valid (revoked / expired server-side).
				Self->AuthToken.Reset();
				Self->Profile = FPFBackendProfile();
				IFileManager::Get().Delete(*Self->AuthFilePath());
				Self->OnStatus.Broadcast(TEXT("Session expired — press LOG IN."));
				Self->OnAuthChanged.Broadcast();
				return;
			}
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp);
			if (Code != 200 || !FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				return;   // transient — keep the cached profile
			}
			Root->TryGetStringField(TEXT("displayName"), Self->Profile.DisplayName);
			Root->TryGetBoolField(TEXT("isChild"), Self->Profile.bIsChild);
			Root->TryGetNumberField(TEXT("xpTotal"), Self->Profile.XpTotal);
			const TSharedPtr<FJsonObject>* Progress = nullptr;
			if (Root->TryGetObjectField(TEXT("progress"), Progress) && Progress)
			{
				(*Progress)->TryGetNumberField(TEXT("level"), Self->Profile.Level);
				(*Progress)->TryGetNumberField(TEXT("into"), Self->Profile.XpInto);
				(*Progress)->TryGetNumberField(TEXT("next"), Self->Profile.XpNext);
			}
			const TSharedPtr<FJsonObject>* Stats = nullptr;
			if (Root->TryGetObjectField(TEXT("stats"), Stats) && Stats)
			{
				(*Stats)->TryGetNumberField(TEXT("matches"), Self->Profile.Matches);
				(*Stats)->TryGetNumberField(TEXT("wins"), Self->Profile.Wins);
				(*Stats)->TryGetNumberField(TEXT("eliminations"), Self->Profile.Eliminations);
			}
			Self->SaveAuthToDisk();   // keeps the cached displayName fresh for next boot
			Self->OnAuthChanged.Broadcast();
		});
}

void UPFBackendSubsystem::LinkInstallGuid()
{
	const UCombatForgeGameInstance* GI = Cast<UCombatForgeGameInstance>(GetGameInstance());
	if (!GI || !IsLoggedIn())
	{
		return;
	}
	const FString Body = FString::Printf(TEXT("{\"guidHash\":\"%s\"}"), *GI->GetLocalPlayerGuidHash());
	Request(TEXT("POST"), TEXT("/v1/identities/link"), Body, /*AuthMode=*/1, nullptr);
}

// ---------------------------------------------------------------------------
// Server directory (player hat)
// ---------------------------------------------------------------------------

bool UPFBackendSubsystem::ParseServerInfo(const TSharedPtr<FJsonObject>& Obj, FPFBackendServerInfo& Out)
{
	if (!Obj.IsValid())
	{
		return false;
	}
	Obj->TryGetStringField(TEXT("server_id"), Out.ServerId);
	Obj->TryGetStringField(TEXT("name"), Out.Name);
	Obj->TryGetStringField(TEXT("addr"), Out.Addr);
	Obj->TryGetStringField(TEXT("lan_addr"), Out.LanAddr);
	Obj->TryGetNumberField(TEXT("port"), Out.Port);
	Obj->TryGetStringField(TEXT("map"), Out.Map);
	Obj->TryGetStringField(TEXT("mode"), Out.Mode);
	Obj->TryGetStringField(TEXT("format"), Out.Format);
	Obj->TryGetStringField(TEXT("phase"), Out.Phase);
	Obj->TryGetNumberField(TEXT("players"), Out.Players);
	Obj->TryGetNumberField(TEXT("max_players"), Out.MaxPlayers);
	Obj->TryGetNumberField(TEXT("net_protocol"), Out.NetProtocol);
	return !Out.Addr.IsEmpty() && Out.Port > 0;
}

void UPFBackendSubsystem::FetchServers(TFunction<void(bool, const TArray<FPFBackendServerInfo>&)> Done)
{
	Request(TEXT("GET"), TEXT("/v1/servers"), FString(), /*AuthMode=*/1,
		[Done](int32 Code, const FString& Resp)
		{
			TArray<FPFBackendServerInfo> Servers;
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp);
			const bool bOk = Code == 200 && FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid();
			if (bOk)
			{
				const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
				if (Root->TryGetArrayField(TEXT("servers"), Rows) && Rows)
				{
					for (const TSharedPtr<FJsonValue>& Row : *Rows)
					{
						FPFBackendServerInfo Info;
						if (ParseServerInfo(Row->AsObject(), Info))
						{
							Servers.Add(MoveTemp(Info));
						}
					}
				}
			}
			if (Done)
			{
				Done(bOk, Servers);
			}
		});
}

void UPFBackendSubsystem::RequestQuickPlay(TFunction<void(bool, const FPFBackendServerInfo&)> Done)
{
	const FString Path = FString::Printf(TEXT("/v1/quickplay?netProtocol=%d"), PFBuild::NetProtocol);
	Request(TEXT("GET"), Path, FString(), /*AuthMode=*/1,
		[Done](int32 Code, const FString& Resp)
		{
			FPFBackendServerInfo Info;
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp);
			bool bOk = Code == 200 && FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid();
			const TSharedPtr<FJsonObject>* Server = nullptr;
			bOk = bOk && Root->TryGetObjectField(TEXT("server"), Server) && Server
				&& ParseServerInfo(*Server, Info);
			if (Done)
			{
				Done(bOk, Info);
			}
		});
}

void UPFBackendSubsystem::RequestJoinByCode(const FString& Code,
	TFunction<void(bool, const FPFBackendServerInfo&)> Done)
{
	const FString Clean = Code.TrimStartAndEnd().ToUpper();
	Request(TEXT("GET"), FString::Printf(TEXT("/v1/join/%s"), *Clean), FString(), /*AuthMode=*/1,
		[Done](int32 RespCode, const FString& Resp)
		{
			FPFBackendServerInfo Info;
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp);
			bool bOk = RespCode == 200 && FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid();
			const TSharedPtr<FJsonObject>* Server = nullptr;
			bOk = bOk && Root->TryGetObjectField(TEXT("server"), Server) && Server
				&& ParseServerInfo(*Server, Info);
			if (Done)
			{
				Done(bOk, Info);
			}
		});
}

// ---------------------------------------------------------------------------
// Fleet hat (dedicated + provisioned key only)
// ---------------------------------------------------------------------------

void UPFBackendSubsystem::FleetRegisterIfServer(UWorld* World)
{
	// The KEY is the trust boundary, not the net mode: a true dedicated binary and the Phase-0
	// pilot (game exe + `?listen` + -nullrhi, per Deploy/playtest/run-server.ps1) both qualify —
	// player installs never have a key, so a random listen host can never register or grant XP.
	const ENetMode Net = World ? World->GetNetMode() : NM_Standalone;
	if (!World || (Net != NM_DedicatedServer && Net != NM_ListenServer)
		|| ServerKey.IsEmpty() || bFleetRegistered)
	{
		return;
	}
	FleetGS = World->GetGameState<ACombatForgeGameState>();
	FleetPort = World->URL.Port;

	const FString Body = FString::Printf(
		TEXT("{\"port\":%d,\"netProtocol\":%d,\"maxPlayers\":%d,\"map\":\"%s\",\"mode\":\"%s\"}"),
		FleetPort, PFBuild::NetProtocol, (int32)PFGrid::MaxRosterSlots,
		FleetGS.IsValid() ? *EnumShortName(TEXT("/Script/CombatForge.EPFArenaMap"), (int64)FleetGS->ArenaMap) : TEXT(""),
		FleetGS.IsValid() ? *EnumShortName(TEXT("/Script/CombatForge.EPFMatchType"), (int64)FleetGS->MatchType) : TEXT(""));

	TWeakObjectPtr<UPFBackendSubsystem> WeakThis(this);
	Request(TEXT("POST"), TEXT("/v1/servers/register"), Body, /*AuthMode=*/2,
		[WeakThis](int32 Code, const FString& Resp)
		{
			UPFBackendSubsystem* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}
			if (Code != 200)
			{
				UE_LOG(CombatForgeLog, Warning, TEXT("Backend: fleet register failed (%d) %s"), Code, *Resp);
				return;
			}
			Self->bFleetRegistered = true;
			UE_LOG(CombatForgeLog, Log, TEXT("Backend: fleet registered (port %d)"), Self->FleetPort);
			Self->HeartbeatTicker = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([WeakThis](float) -> bool
				{
					if (UPFBackendSubsystem* Inner = WeakThis.Get())
					{
						Inner->SendHeartbeat();
						return true;
					}
					return false;
				}), HeartbeatPeriodSec);
			Self->SendHeartbeat();

			// Crash recovery: re-send any match reports that never reached the API. The Worker is
			// idempotent on matchId, so double delivery is harmless (progression-plan §1).
			TArray<FString> Pending;
			const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("PendingReports"));
			IFileManager::Get().FindFiles(Pending, *FPaths::Combine(Dir, TEXT("*.json")), true, false);
			for (const FString& File : Pending)
			{
				FString Json;
				const FString FullPath = FPaths::Combine(Dir, File);
				if (FFileHelper::LoadFileToString(Json, *FullPath))
				{
					Self->SendMatchReport(Json, FullPath);
				}
			}
		});
}

void UPFBackendSubsystem::SendHeartbeat()
{
	const ACombatForgeGameState* GS = FleetGS.Get();
	if (!bFleetRegistered)
	{
		return;
	}
	// Re-resolve after map reload (`open L_Graybox` recreates the GameState).
	if (!GS)
	{
		if (const UGameInstance* GI = GetGameInstance())
		{
			if (const UWorld* World = GI->GetWorld())
			{
				FleetGS = World->GetGameState<ACombatForgeGameState>();
				GS = FleetGS.Get();
			}
		}
	}
	int32 Humans = 0;
	FString PhaseStr, MapStr, ModeStr;
	if (GS)
	{
		for (const APlayerState* PSBase : GS->PlayerArray)
		{
			const ACombatForgePlayerState* PS = Cast<ACombatForgePlayerState>(PSBase);
			if (PS && !PS->IsABot() && !PS->IsHeadlessServerPhantom())
			{
				++Humans;   // the pilot box's own phantom player is not a "player" in the browser
			}
		}
		PhaseStr = EnumShortName(TEXT("/Script/CombatForge.EPFMatchPhase"), (int64)GS->Phase);
		MapStr   = GS->SelectedCommunityMapLabel.IsEmpty()
			? EnumShortName(TEXT("/Script/CombatForge.EPFArenaMap"), (int64)GS->ArenaMap)
			: GS->SelectedCommunityMapLabel;
		ModeStr  = EnumShortName(TEXT("/Script/CombatForge.EPFMatchType"), (int64)GS->MatchType);
	}
	const FString Body = FString::Printf(
		TEXT("{\"players\":%d,\"phase\":\"%s\",\"map\":\"%s\",\"mode\":\"%s\"}"),
		Humans, *PhaseStr, *MapStr.ReplaceQuotesWithEscapedQuotes(), *ModeStr);
	Request(TEXT("POST"), TEXT("/v1/servers/heartbeat"), Body, /*AuthMode=*/2,
		[](int32 Code, const FString&)
		{
			if (Code == 409)
			{
				UE_LOG(CombatForgeLog, Warning, TEXT("Backend: heartbeat says not-registered (409)"));
			}
		});
}

void UPFBackendSubsystem::SendMatchReport(const FString& ReportJson, const FString& PendingFilePath)
{
	if (ServerKey.IsEmpty())
	{
		return;   // by design: only provisioned fleet boxes can grant XP
	}
	const int64 Ts = FDateTime::UtcNow().ToUnixTimestamp();
	const FString Signature = HmacSha256Hex(ServerKey,
		FString::Printf(TEXT("%lld.%s"), Ts, *ReportJson));
	TMap<FString, FString> Headers;
	Headers.Add(TEXT("X-Timestamp"), FString::Printf(TEXT("%lld"), Ts));
	Headers.Add(TEXT("X-Signature"), Signature);
	Request(TEXT("POST"), TEXT("/v1/match-report"), ReportJson, /*AuthMode=*/2,
		[PendingFilePath](int32 Code, const FString& Resp)
		{
			if (Code == 200)
			{
				UE_LOG(CombatForgeLog, Log, TEXT("Backend: match report accepted: %s"), *Resp);
				if (!PendingFilePath.IsEmpty())
				{
					IFileManager::Get().Delete(*PendingFilePath);
				}
			}
			else
			{
				// Leave the pending file in place — re-sent at next fleet registration.
				UE_LOG(CombatForgeLog, Warning, TEXT("Backend: match report failed (%d) %s"), Code, *Resp);
			}
		}, Headers);
}
