// Copyright (c) 2026 Tom Chapman. All rights reserved.

#include "Online/PFBackendSubsystem.h"

#include "CombatForge.h"
#include "Core/CombatForgeGameInstance.h"
#include "Core/CombatForgeGameState.h"
#include "Core/CombatForgePlayerState.h"
#include "Combat/PFWeaponCatalog.h"
#include "Core/PFPaths.h"

#include "Dom/JsonObject.h"
#include "GameFramework/PlayerState.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/CommandLine.h"
#include "Misc/Base64.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <wincrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

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

#if PLATFORM_WINDOWS
	bool ProtectPlayerToken(const FString& Plaintext, FString& OutProtected)
	{
		const FTCHARToUTF8 Utf8(*Plaintext);
		DATA_BLOB Input{};
		Input.cbData = static_cast<DWORD>(Utf8.Length());
		Input.pbData = reinterpret_cast<BYTE*>(const_cast<ANSICHAR*>(Utf8.Get()));
		DATA_BLOB Output{};
		if (!CryptProtectData(&Input, TEXT("CombatForge player session"), nullptr, nullptr, nullptr,
			CRYPTPROTECT_UI_FORBIDDEN, &Output))
		{
			return false;
		}
		OutProtected = FBase64::Encode(Output.pbData, static_cast<uint32>(Output.cbData));
		FMemory::Memzero(Output.pbData, Output.cbData);
		LocalFree(Output.pbData);
		return !OutProtected.IsEmpty();
	}

	bool UnprotectPlayerToken(const FString& Protected, FString& OutPlaintext)
	{
		TArray<uint8> Ciphertext;
		if (!FBase64::Decode(Protected, Ciphertext) || Ciphertext.IsEmpty())
		{
			return false;
		}
		DATA_BLOB Input{};
		Input.cbData = static_cast<DWORD>(Ciphertext.Num());
		Input.pbData = Ciphertext.GetData();
		DATA_BLOB Output{};
		if (!CryptUnprotectData(&Input, nullptr, nullptr, nullptr, nullptr,
			CRYPTPROTECT_UI_FORBIDDEN, &Output))
		{
			return false;
		}
		const FUTF8ToTCHAR Decoded(reinterpret_cast<const ANSICHAR*>(Output.pbData), Output.cbData);
		OutPlaintext = FString(Decoded.Length(), Decoded.Get());
		FMemory::Memzero(Output.pbData, Output.cbData);
		LocalFree(Output.pbData);
		return !OutPlaintext.IsEmpty();
	}
#endif

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

	constexpr float FleetRegisterRetryInitialSec = 10.f;
	constexpr float FleetRegisterRetryMidSec     = 30.f;
	constexpr float FleetRegisterRetryMaxSec     = 60.f;

	// Four 0..255 octets, nothing else. Local checks (not a test harness):
	//   "192.168.1.5" ok; "192.168.1.5:7777" / "2001:db8::1" / "box.local" / "" fail
	//   (192.168.1.5 >> 8) != (192.168.10.5 >> 8)  — last-dot /24 is not the rule
	//   IsRfc1918: 10/8, 172.16/12, 192.168/16; 203.0.113.5 false
	bool ParseIpv4Host(const FString& S, uint32& Out)
	{
		TArray<FString> Octets;
		S.ParseIntoArray(Octets, TEXT("."), /*bCullEmpty=*/false);
		if (Octets.Num() != 4)
		{
			return false;
		}
		uint32 Acc = 0;
		for (const FString& Octet : Octets)
		{
			if (Octet.IsEmpty() || Octet.Len() > 3)
			{
				return false;
			}
			int32 Value = 0;
			for (const TCHAR C : Octet)
			{
				if (C < TEXT('0') || C > TEXT('9'))
				{
					return false;
				}
				Value = Value * 10 + (C - TEXT('0'));
			}
			if (Value > 255)
			{
				return false;
			}
			Acc = (Acc << 8) | static_cast<uint32>(Value);
		}
		Out = Acc;
		return true;
	}

	bool IsRfc1918(uint32 Ip)
	{
		const uint32 B0 = (Ip >> 24) & 0xFFu;
		const uint32 B1 = (Ip >> 16) & 0xFFu;
		return B0 == 10u
			|| (B0 == 172u && B1 >= 16u && B1 <= 31u)
			|| (B0 == 192u && B1 == 168u);
	}
}

FString FPFBackendServerInfo::JoinAddress() const
{
	// Prefer LanAddr only when the row also has a distinct public mapping: both the
	// local adapter and LanAddr parse as IPv4, share a real /24, LanAddr is
	// RFC1918, and Addr is a different public IPv4. The public mapping proves this
	// row came from our server; matching a common private /24 alone is not "this LAN".
	if (!LanAddr.IsEmpty())
	{
		if (ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			bool bCanBind = false;
			const TSharedRef<FInternetAddr> Local = Sockets->GetLocalHostAddr(*GLog, bCanBind);
			const FString LocalIp = Local->ToString(/*bAppendPort=*/false);
			uint32 LocalHost = 0, LanHost = 0;
			uint32 PubHost = 0;
			const bool bHasDistinctPublic = ParseIpv4Host(Addr, PubHost)
				&& !IsRfc1918(PubHost)
				&& Addr != LanAddr;
			if (bCanBind
				&& ParseIpv4Host(LocalIp, LocalHost) && ParseIpv4Host(LanAddr, LanHost)
				&& (LocalHost >> 8) == (LanHost >> 8)
				&& IsRfc1918(LanHost)
				&& bHasDistinctPublic)
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

	// The LAN-only public Alpha must never turn a cached player token or an old server-key file into a
	// hidden dependency. Leave both identities untouched on disk for the future service, but do not load
	// either one and therefore do not issue account, progression, directory, or fleet requests.
	if (!PFBuild::OfficialServersEnabled)
	{
		AuthToken.Reset();
		ServerKey.Reset();
		Profile = FPFBackendProfile();
		UE_LOG(CombatForgeLog, Log, TEXT("Backend: official service disabled for LAN-only Alpha"));
		return;
	}

	// Fleet key (dedicated boxes only): a development-only command-line override, then
	// <ServerDataDir>/ServerKey.txt. Shipping never accepts the secret via argv because process lists and
	// UE startup logs expose command lines. ServerDataDir is a PERSISTENT dir (honors -ArenaDir on the box)
	// — NOT ProjectSavedDir, which is inside the package and gets wiped on every redeploy.
#if !UE_BUILD_SHIPPING
	FParse::Value(FCommandLine::Get(), TEXT("PFServerKey="), ServerKey);
#endif
	if (ServerKey.IsEmpty())
	{
		FString FromDisk;
		if (FFileHelper::LoadFileToString(FromDisk,
			*FPaths::Combine(FPFPaths::ServerDataDir(), TEXT("ServerKey.txt"))))
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
	FleetUnregister();   // best-effort: drop off the directory instead of ghosting for 30 s
	Super::Deinitialize();
}

FString UPFBackendSubsystem::AuthFilePath() const
{
	// PER-USER, per-machine location — NOT ProjectSavedDir. For a portable/packaged build run from a writable
	// folder, ProjectSavedDir resolves to INSIDE the package, so the session token would land in the shippable
	// folder and get distributed to everyone who downloads it (this leaked one login to every alpha download,
	// 2026-07-17). %LOCALAPPDATA%/CombatForge is per-user and never packaged (same root arenas already use).
	return FPaths::Combine(FString(FPlatformProcess::UserSettingsDir()), TEXT("CombatForge"), TEXT("Auth.json"));
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
#if PLATFORM_WINDOWS
		bool bLoadedLegacyPlaintext = false;
		FString ProtectedToken;
		if (Root->TryGetStringField(TEXT("protectedToken"), ProtectedToken))
		{
			if (!UnprotectPlayerToken(ProtectedToken, AuthToken))
			{
				UE_LOG(CombatForgeLog, Warning, TEXT("Backend: stored player session could not be decrypted; deleting it"));
				AuthToken.Reset();
				IFileManager::Get().Delete(*AuthFilePath());
				return;
			}
			}
			else
			{
				bLoadedLegacyPlaintext = Root->TryGetStringField(TEXT("token"), AuthToken);
			}
#else
		Root->TryGetStringField(TEXT("token"), AuthToken);
#endif
		Root->TryGetStringField(TEXT("displayName"), Profile.DisplayName);
#if PLATFORM_WINDOWS
		// One-time migration of alpha.19's plaintext file. If DPAPI is unavailable, fail closed and
		// remove the bearer token instead of leaving the legacy secret readable on disk.
		if (bLoadedLegacyPlaintext && !AuthToken.IsEmpty() && !SaveAuthToDisk())
		{
			AuthToken.Reset();
			IFileManager::Get().Delete(*AuthFilePath());
		}
#endif
	}
}

bool UPFBackendSubsystem::SaveAuthToDisk() const
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
#if PLATFORM_WINDOWS
	FString ProtectedToken;
	if (!AuthToken.IsEmpty() && !ProtectPlayerToken(AuthToken, ProtectedToken))
	{
		UE_LOG(CombatForgeLog, Error, TEXT("Backend: DPAPI could not protect the player session; not writing it"));
		return false;
	}
	Root->SetNumberField(TEXT("schema"), 2);
	Root->SetStringField(TEXT("protectedToken"), ProtectedToken);
#else
	Root->SetNumberField(TEXT("schema"), 1);
	Root->SetStringField(TEXT("token"), AuthToken);
#endif
	Root->SetStringField(TEXT("displayName"), Profile.DisplayName);
	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Root, Writer);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(AuthFilePath()), /*Tree=*/true);
	return FFileHelper::SaveStringToFile(Output, *AuthFilePath());
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
	// ProcessRequest() == false does NOT mean "nothing happened": UE 5.6's FHttpRequestCommon::
	// PreProcess calls FinishRequestNotInHttpManager() on a failed PreCheck/SetupRequest, and on the
	// game thread (the default CompleteOnGameThread policy) that fires OnProcessRequestComplete
	// SYNCHRONOUSLY before ProcessRequest returns. A bare "if (!ProcessRequest()) Done(0, ...)"
	// therefore delivers the SAME failure twice — which double-decrements SendMatchReport's
	// PendingReplayInFlight latch and lets a still-in-flight report be replayed. One shared latch,
	// so exactly one of the two paths ever calls Done.
	const TSharedRef<bool> bDelivered = MakeShared<bool>(false);
	Req->OnProcessRequestComplete().BindLambda(
		[Done, bDelivered](FHttpRequestPtr /*R*/, FHttpResponsePtr Resp, bool bOk)
		{
			if (Done && !*bDelivered)
			{
				*bDelivered = true;
				Done(bOk && Resp.IsValid() ? Resp->GetResponseCode() : 0,
					bOk && Resp.IsValid() ? Resp->GetContentAsString() : FString());
			}
		});
	if (!Req->ProcessRequest() && Done && !*bDelivered)
	{
		*bDelivered = true;
		Done(0, FString());
	}
}

// ---------------------------------------------------------------------------
// Device-link login (player hat)
// ---------------------------------------------------------------------------

void UPFBackendSubsystem::BeginDeviceLogin()
{
	if (!PFBuild::OfficialServersEnabled)
	{
		OnStatus.Broadcast(TEXT("No official servers are available in this Alpha. LAN play needs no account."));
		return;
	}
	if (IsDeviceLoginActive())
	{
		return;   // covers the in-flight window too — a double-click can't start two flows
	}
	bDeviceCodeRequestInFlight = true;
	const int32 Gen = ++DeviceFlowGeneration;
	OnStatus.Broadcast(TEXT("Contacting the account server…"));
	const FString Body = FString::Printf(TEXT("{\"client_id\":\"%s\"}"), GameClientId);
	TWeakObjectPtr<UPFBackendSubsystem> WeakThis(this);
	Request(TEXT("POST"), TEXT("/api/auth/device/code"), Body, /*AuthMode=*/0,
		[WeakThis, Gen](int32 Code, const FString& Resp)
		{
			UPFBackendSubsystem* Self = WeakThis.Get();
			if (!Self || Self->DeviceFlowGeneration != Gen)
			{
				return;   // canceled while in flight — a stale response must not resurrect the flow
			}
			Self->bDeviceCodeRequestInFlight = false;
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp);
			if (Code != 200 || !FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				Self->OnStatus.Broadcast(TEXT("Couldn't reach the account server — try again."));
				return;
			}
			Root->TryGetStringField(TEXT("device_code"), Self->PendingDeviceCode);
			Root->TryGetStringField(TEXT("user_code"), Self->PendingUserCode);
			// The approval URL comes from the API — never hardcode it (the /link page lives on the
			// API host; the main site only gains a redirect when Tom adds one).
			Self->VerificationUri = TEXT("the CombatForge website");
			Root->TryGetStringField(TEXT("verification_uri"), Self->VerificationUri);
			double Interval = 5.0, ExpiresIn = 900.0;
			Root->TryGetNumberField(TEXT("interval"), Interval);
			Root->TryGetNumberField(TEXT("expires_in"), ExpiresIn);
			Self->DevicePollIntervalSec = FMath::Clamp((float)Interval, 2.f, 30.f);
			Self->DeviceExpiresAtSec = FPlatformTime::Seconds() + ExpiresIn;
			// Explicit about the sign-up step — the approval page's own "New here?" link is easy to
			// miss on a first visit if nothing set the expectation (Tom 2026-07-17: had a code, no
			// idea an account was needed or how to make one).
			Self->OnStatus.Broadcast(FString::Printf(
				TEXT("Code %s — go to %s (no account yet? you can sign up free there)"),
				*Self->PendingUserCode, *Self->VerificationUri));
			Self->ArmDevicePollTicker();
		});
}

void UPFBackendSubsystem::ArmDevicePollTicker()
{
	if (DevicePollTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DevicePollTicker);
		DevicePollTicker.Reset();
	}
	TWeakObjectPtr<UPFBackendSubsystem> WeakThis(this);
	DevicePollTicker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([WeakThis](float) -> bool
		{
			if (UPFBackendSubsystem* Inner = WeakThis.Get())
			{
				Inner->PollDeviceToken();
				return true;   // keep ticking; StopDevicePolling removes us
			}
			return false;
		}), DevicePollIntervalSec);
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
	if (bTokenPollInFlight)
	{
		return;   // a previous poll is still out on a slow link — don't stack another (P2-ON2)
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
	const int32 Gen = DeviceFlowGeneration;
	bTokenPollInFlight = true;
	TWeakObjectPtr<UPFBackendSubsystem> WeakThis(this);
	Request(TEXT("POST"), TEXT("/api/auth/device/token"), Body, /*AuthMode=*/0,
		[WeakThis, Gen](int32 Code, const FString& Resp)
		{
			UPFBackendSubsystem* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}
			// Clear the in-flight latch for THIS generation only; a StopDevicePolling that already
			// bumped the generation has cleared it, and a newer poll may own it now.
			if (Self->DeviceFlowGeneration == Gen)
			{
				Self->bTokenPollInFlight = false;
			}
			if (Self->DeviceFlowGeneration != Gen || Self->PendingDeviceCode.IsEmpty())
			{
				return;   // canceled/superseded while this poll was in flight
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
				Self->DevicePollIntervalSec += 5.f;   // RFC 8628 §3.5
				Self->ArmDevicePollTicker();          // re-arm — FTSTicker's delay is fixed at add time
				return;
			}
			Self->StopDevicePolling();
			Self->OnStatus.Broadcast(Err == TEXT("access_denied")
				? TEXT("Link was denied on the website.")
				: TEXT("Log-in failed — press LOG IN to retry."));
		});
}

void UPFBackendSubsystem::StopDevicePolling()
{
	++DeviceFlowGeneration;   // invalidate every in-flight code/token callback
	bDeviceCodeRequestInFlight = false;
	bTokenPollInFlight = false;
	if (DevicePollTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DevicePollTicker);
		DevicePollTicker.Reset();
	}
	PendingDeviceCode.Reset();
	PendingUserCode.Reset();
	// Otherwise a UI that reads GetVerificationUri() after a cancel keeps showing the previous
	// approval URL until the next successful code response. (P2-ON3)
	VerificationUri.Reset();
}

void UPFBackendSubsystem::HandleLoginSucceeded(const FString& Token)
{
	AuthToken = Token;
	StopDevicePolling();
	const bool bStored = SaveAuthToDisk();
	OnStatus.Broadcast(bStored ? TEXT("Signed in!") : TEXT("Signed in for this session; secure storage failed."));
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
	if (!PFBuild::OfficialServersEnabled)
	{
		return;
	}
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
			// Weapon unlocks: API returns ["wpn.ar_m4", ...] (weapon-implementation-spec §7).
			Self->Profile.UnlockIds.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Unlocks = nullptr;
			if (Root->TryGetArrayField(TEXT("unlocks"), Unlocks) && Unlocks)
			{
				for (const TSharedPtr<FJsonValue>& V : *Unlocks)
				{
					if (V.IsValid() && V->Type == EJson::String)
					{
						Self->Profile.UnlockIds.Add(V->AsString());
					}
				}
			}
			Self->SaveAuthToDisk();   // keeps the cached displayName fresh for next boot
			Self->OnAuthChanged.Broadcast();
		});
}

bool UPFBackendSubsystem::IsWeaponUnlocked(const FString& WeaponId) const
{
	// Public Alpha release contract: local/LAN loadouts never depend on the dormant account service,
	// including on a machine that still has an Auth.json from an older test build.
	if (!PFBuild::OfficialServersEnabled)
	{
		return true;
	}
	// Offline / LAN / pre-backend: fully ungated (spec Stage 5).
	if (!IsLoggedIn() || Profile.UnlockIds.Num() == 0 || WeaponId.IsEmpty())
	{
		return true;
	}

	// RANK IS AUTHORITATIVE, the id list is only an additive grant. UnlockIds is whatever the backend
	// happens to have seeded; it does NOT enumerate the rank-1 starters, so keying purely off it showed a
	// rank-10 player "LOCKED - Rank 1" on the default rifle and pistol (Tom 2026-07-20). If you have the
	// rank, you have the gun.
	//
	// Client-side gate only: a fleet server re-checks the real profile in ServerSetKit::ClampSlot,
	// so what this menu allows can never carry a locked gun onto an official server.
	{
		const FString Bare = WeaponId.StartsWith(TEXT("wpn.")) ? WeaponId.RightChop(4) : WeaponId;
		const FPFWeaponConfig C = PFWeapon::FindById(Bare);
		if (PFWeapon::IdOf(C.Category, C.Index).Equals(Bare, ESearchCase::IgnoreCase)
			&& static_cast<int32>(PFWeapon::UnlockRankOf(C.Category, C.Index)) <= EffectiveRank())
		{
			return true;
		}
	}
	const FString Prefixed = WeaponId.StartsWith(TEXT("wpn."))
		? WeaponId
		: FString::Printf(TEXT("wpn.%s"), *WeaponId);
	for (const FString& Id : Profile.UnlockIds)
	{
		if (Id.Equals(Prefixed, ESearchCase::IgnoreCase) || Id.Equals(WeaponId, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
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
	if (!PFBuild::OfficialServersEnabled)
	{
		if (Done) { Done(false, TArray<FPFBackendServerInfo>()); }
		return;
	}
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
	if (!PFBuild::OfficialServersEnabled)
	{
		if (Done) { Done(false, FPFBackendServerInfo()); }
		return;
	}
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
	if (!PFBuild::OfficialServersEnabled)
	{
		if (Done) { Done(false, FPFBackendServerInfo()); }
		return;
	}
	// Join codes are 6-char A-Z0-9 — strip anything else BEFORE the string is Printf'd into a URL path
	// (a pasted "AB/CD?" would otherwise rewrite the request path; issue #18 ON3).
	FString Clean;
	Clean.Reserve(8);
	for (const TCHAR C : Code.TrimStartAndEnd().ToUpper())
	{
		if (FChar::IsAlnum(C))
		{
			Clean.AppendChar(C);
		}
	}
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
	if (!PFBuild::OfficialServersEnabled)
	{
		return;
	}
	// The KEY is the trust boundary, not the net mode: a true dedicated binary and the Phase-0
	// pilot (game exe + `?listen` + -nullrhi, per Deploy/playtest/run-server.ps1) both qualify —
	// player installs never have a key, so a random listen host can never register or grant XP.
	const ENetMode Net = World ? World->GetNetMode() : NM_Standalone;
	if (!World || (Net != NM_DedicatedServer && Net != NM_ListenServer)
		|| ServerKey.IsEmpty() || bFleetRegistered || bFleetRegisterInFlight)
	{
		return;
	}
	bFleetRegisterInFlight = true;
	FleetGS = World->GetGameState<ACombatForgeGameState>();
	FleetPort = World->URL.Port;

	// LAN address rides along so same-LAN joiners skip NAT hairpin (flaky on consumer routers —
	// exactly the Phase-0 pilot-in-Tom's-house case). The Worker still records the OBSERVED
	// public IP as the primary address.
	FString LanAddr;
	if (ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
	{
		bool bCanBind = false;
		const TSharedRef<FInternetAddr> Local = Sockets->GetLocalHostAddr(*GLog, bCanBind);
		LanAddr = Local->ToString(/*bAppendPort=*/false);
	}
	// -PFPrivate: unlisted match — the directory mints a 6-char join code instead of listing us.
	const bool bPrivate = FParse::Param(FCommandLine::Get(), TEXT("PFPrivate"));

	const FString Body = FString::Printf(
		TEXT("{\"port\":%d,\"netProtocol\":%d,\"maxPlayers\":%d,\"map\":\"%s\",\"mode\":\"%s\",")
		TEXT("\"lanAddr\":\"%s\",\"private\":%s}"),
		FleetPort, PFBuild::NetProtocol, (int32)PFGrid::MaxRosterSlots,
		FleetGS.IsValid() ? *EnumShortName(TEXT("/Script/CombatForge.EPFArenaMap"), (int64)FleetGS->ArenaMap) : TEXT(""),
		FleetGS.IsValid() ? *EnumShortName(TEXT("/Script/CombatForge.EPFMatchType"), (int64)FleetGS->MatchType) : TEXT(""),
		*LanAddr, bPrivate ? TEXT("true") : TEXT("false"));

	TWeakObjectPtr<UPFBackendSubsystem> WeakThis(this);
	Request(TEXT("POST"), TEXT("/v1/servers/register"), Body, /*AuthMode=*/2,
		[WeakThis](int32 Code, const FString& Resp)
		{
			UPFBackendSubsystem* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}
			Self->bFleetRegisterInFlight = false;
			if (Self->bFleetRegistered)
			{
				return;   // a parallel register already won — don't arm a second heartbeat
			}
			if (Code != 200)
			{
				UE_LOG(CombatForgeLog, Warning, TEXT("Backend: fleet register failed (%d) %s"), Code, *Resp);
				// Transient (cold Worker / 409 "already listed" / timeout): bounded backoff.
				// Config 4xx (400/401/403 and other non-retry 4xx): stay dark — do not hammer a bad key.
				const bool bTransient = Code == 0 || Code == 408 || Code == 409 || Code == 429 || Code >= 500;
				if (bTransient)
				{
					Self->ArmFleetRegisterRetry();
				}
				return;
			}
			// Private match: surface the join code where the box operator can read + share it.
			TSharedPtr<FJsonObject> RegRoot;
			const TSharedRef<TJsonReader<>> RegReader = TJsonReaderFactory<>::Create(Resp);
			FString JoinCode;
			if (FJsonSerializer::Deserialize(RegReader, RegRoot) && RegRoot.IsValid()
				&& RegRoot->TryGetStringField(TEXT("joinCode"), JoinCode) && !JoinCode.IsEmpty())
			{
				UE_LOG(CombatForgeLog, Display, TEXT("Backend: PRIVATE MATCH CODE: %s"), *JoinCode);
				FFileHelper::SaveStringToFile(JoinCode, *FPaths::Combine(
					FPFPaths::ServerDataDir(), TEXT("JoinCode.txt")));
			}
			Self->bFleetRegistered = true;
			Self->FleetRegisterRetryDelaySec = FleetRegisterRetryInitialSec;
			if (Self->FleetRegisterRetryTicker.IsValid())
			{
				FTSTicker::GetCoreTicker().RemoveTicker(Self->FleetRegisterRetryTicker);
				Self->FleetRegisterRetryTicker.Reset();
			}
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
			Self->ReplayPendingReports();
		});
}

void UPFBackendSubsystem::ReplayPendingReports()
{
	if (PendingReplayInFlight > 0)
	{
		return;
	}
	// Crash / mid-session recovery: re-send reports that never reached the API. Worker is
	// idempotent on matchId. Persistent dir so a redeploy mid-unsent-report doesn't drop XP.
	// FindFiles is non-recursive — rejected/ is not walked.
	TArray<FString> Pending;
	const FString Dir = FPaths::Combine(FPFPaths::ServerDataDir(), TEXT("PendingReports"));
	IFileManager::Get().FindFiles(Pending, *FPaths::Combine(Dir, TEXT("*.json")), true, false);
	for (const FString& File : Pending)
	{
		FString Json;
		const FString FullPath = FPaths::Combine(Dir, File);
		if (FFileHelper::LoadFileToString(Json, *FullPath))
		{
			SendMatchReport(Json, FullPath);
		}
	}
}

void UPFBackendSubsystem::ArmFleetRegisterRetry()
{
	if (FleetRegisterRetryTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(FleetRegisterRetryTicker);
		FleetRegisterRetryTicker.Reset();
	}
	const float Delay = FleetRegisterRetryDelaySec;
	FleetRegisterRetryDelaySec = (Delay < FleetRegisterRetryMidSec)
		? FleetRegisterRetryMidSec
		: FleetRegisterRetryMaxSec;
	TWeakObjectPtr<UPFBackendSubsystem> WeakThis(this);
	FleetRegisterRetryTicker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([WeakThis](float) -> bool
		{
			UPFBackendSubsystem* Inner = WeakThis.Get();
			if (!Inner)
			{
				return false;
			}
			Inner->FleetRegisterRetryTicker.Reset();
			const UGameInstance* GI = Inner->GetGameInstance();
			const UWorld* World = GI ? GI->GetWorld() : nullptr;
			const ENetMode Net = World ? World->GetNetMode() : NM_Standalone;
			if (Net != NM_DedicatedServer && Net != NM_ListenServer)
			{
				return false;   // STOP HOSTING — do not re-arm
			}
			Inner->FleetRegisterIfServer(GI->GetWorld());
			return false;       // one-shot; fail path re-arms from the register callback
		}), Delay);
}

void UPFBackendSubsystem::SendHeartbeat()
{
	const ACombatForgeGameState* GS = FleetGS.Get();
	if (!bFleetRegistered)
	{
		return;
	}
	// Ghost-server guard: if this process is no longer actually SERVING (the box operator hit
	// STOP HOSTING, or the world dropped to standalone), fall out of the directory instead of
	// heartbeating quick-play traffic at a dead port.
	if (const UGameInstance* GI = GetGameInstance())
	{
		const UWorld* World = GI->GetWorld();
		const ENetMode Net = World ? World->GetNetMode() : NM_Standalone;
		if (Net != NM_DedicatedServer && Net != NM_ListenServer)
		{
			UE_LOG(CombatForgeLog, Log, TEXT("Backend: no longer hosting — unregistering from the directory"));
			FleetUnregister();
			return;
		}
	}
	ReplayPendingReports();
	// ALWAYS re-resolve the current GameState (not just when null): a map reload swaps it, and a stale-but-
	// valid pointer would keep counting the OLD (empty) PlayerArray — the "0/12 with players connected" bug.
	if (const UGameInstance* GI = GetGameInstance())
	{
		if (const UWorld* World = GI->GetWorld())
		{
			FleetGS = World->GetGameState<ACombatForgeGameState>();
		}
	}
	GS = FleetGS.Get();
	int32 Humans = 0;
	FString PhaseStr, MapStr, ModeStr, FormatStr;
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
		// Cycle position rides the mode label so the browser row shows where the wheel is
		// ("Skirmish · Remix Swap") with zero directory-schema changes. During Lobby the stage
		// already advertises the UPCOMING match (advanced at Results).
		if (GS->BuildMode != EPFBuildMode::PlayOnly && GS->MatchType != EPFMatchType::FreeForAll)
		{
			ModeStr = FString::Printf(TEXT("%s · %s"), *ModeStr, PFCycleStageLabel(GS->CycleStage));
		}
		FormatStr = FString::Printf(TEXT("%dv%d"), GS->TargetTeamSize, GS->TargetTeamSize);
	}
	// Proper JSON serialization: community-map labels are free text (quotes/backslashes would
	// silently 400 a Printf-built body and drop us off the directory).
	const TSharedRef<FJsonObject> BodyObj = MakeShared<FJsonObject>();
	// Port identifies WHICH instance of a multi-instance fleet box this beat belongs to — the
	// directory keys per-instance rows as <serverId>#<port> when several share one fleet key.
	BodyObj->SetNumberField(TEXT("port"), FleetPort);
	BodyObj->SetNumberField(TEXT("players"), Humans);
	BodyObj->SetStringField(TEXT("phase"), PhaseStr);
	BodyObj->SetStringField(TEXT("map"), MapStr);
	BodyObj->SetStringField(TEXT("mode"), ModeStr);
	BodyObj->SetStringField(TEXT("format"), FormatStr);
	FString Body;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(BodyObj, Writer);
	TWeakObjectPtr<UPFBackendSubsystem> WeakThis(this);
	Request(TEXT("POST"), TEXT("/v1/servers/heartbeat"), Body, /*AuthMode=*/2,
		[WeakThis](int32 Code, const FString&)
		{
			if (Code != 409)
			{
				return;
			}
			// 409 = the Worker no longer has our row (D1 expiry / wipe / API redeploy). Logging alone left
			// a LIVE box heartbeating 409 forever and invisible in the server browser (issue #18 ON1):
			// bFleetRegistered stayed true, so FleetRegisterIfServer early-returned for the rest of the
			// process lifetime. Recover: drop registered state + the ticker and re-register (which re-arms
			// the heartbeat and re-sends pending match reports). NO /unregister call — the row is gone.
			UPFBackendSubsystem* Self = WeakThis.Get();
			if (Self == nullptr)
			{
				return;
			}
			UE_LOG(CombatForgeLog, Warning,
				TEXT("Backend: heartbeat says not-registered (409) — re-registering with the directory"));
			if (Self->HeartbeatTicker.IsValid())
			{
				FTSTicker::GetCoreTicker().RemoveTicker(Self->HeartbeatTicker);
				Self->HeartbeatTicker.Reset();
			}
			Self->bFleetRegistered = false;
			if (UWorld* ReworldWorld = Self->FleetGS.IsValid() ? Self->FleetGS->GetWorld() : nullptr)
			{
				Self->FleetRegisterIfServer(ReworldWorld);
			}
			else if (UGameInstance* GI = Self->GetGameInstance())
			{
				Self->FleetRegisterIfServer(GI->GetWorld());
			}
		});
}

void UPFBackendSubsystem::FleetUnregister()
{
	if (HeartbeatTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(HeartbeatTicker);
		HeartbeatTicker.Reset();
	}
	if (FleetRegisterRetryTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(FleetRegisterRetryTicker);
		FleetRegisterRetryTicker.Reset();
	}
	if (!bFleetRegistered)
	{
		return;
	}
	bFleetRegistered = false;
	FleetGS.Reset();
	Request(TEXT("POST"), TEXT("/v1/servers/unregister"),
		FString::Printf(TEXT("{\"port\":%d}"), FleetPort), /*AuthMode=*/2, nullptr);
}

void UPFBackendSubsystem::SendMatchReport(const FString& ReportJson, const FString& PendingFilePath)
{
	if (ServerKey.IsEmpty())
	{
		return;   // by design: only provisioned fleet boxes can grant XP
	}
	// Hold the replay latch for every pending-file POST (live GameMode emit and Replay).
	// Decrement only in this request's callback so a 15 s timeout cannot clear a still-in-flight drain.
	if (!PendingFilePath.IsEmpty())
	{
		++PendingReplayInFlight;
	}
	const int64 Ts = FDateTime::UtcNow().ToUnixTimestamp();
	const FString Signature = HmacSha256Hex(ServerKey,
		FString::Printf(TEXT("%lld.%s"), Ts, *ReportJson));
	TMap<FString, FString> Headers;
	Headers.Add(TEXT("X-Timestamp"), FString::Printf(TEXT("%lld"), Ts));
	Headers.Add(TEXT("X-Signature"), Signature);
	TWeakObjectPtr<UPFBackendSubsystem> WeakThis(this);
	Request(TEXT("POST"), TEXT("/v1/match-report"), ReportJson, /*AuthMode=*/2,
		[WeakThis, PendingFilePath](int32 Code, const FString& Resp)
		{
			if (Code == 200)
			{
				UE_LOG(CombatForgeLog, Log, TEXT("Backend: match report accepted: %s"), *Resp);
				if (!PendingFilePath.IsEmpty())
				{
					IFileManager::Get().Delete(*PendingFilePath);
				}
			}
			else if (Code >= 400 && Code < 500 && Code != 408 && Code != 429 && !PendingFilePath.IsEmpty())
			{
				// Deterministic rejection (validation/clamp): quarantine instead of re-signing and
				// re-POSTing the same doomed report on every boot forever.
				const FString Rejected = FPaths::Combine(FPaths::GetPath(PendingFilePath),
					TEXT("rejected"), FPaths::GetCleanFilename(PendingFilePath));
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(Rejected), /*Tree=*/true);
				IFileManager::Get().Move(*Rejected, *PendingFilePath);
				UE_LOG(CombatForgeLog, Warning,
					TEXT("Backend: match report rejected (%d) %s — quarantined to %s"), Code, *Resp, *Rejected);
			}
			else
			{
				// Transient (5xx / network / rate limit): leave in place — replayed on heartbeat / register.
				UE_LOG(CombatForgeLog, Warning, TEXT("Backend: match report failed (%d) %s"), Code, *Resp);
			}
			if (UPFBackendSubsystem* Self = WeakThis.Get())
			{
				if (Self->PendingReplayInFlight > 0)
				{
					--Self->PendingReplayInFlight;
				}
			}
		}, Headers);
}

void UPFBackendSubsystem::SendCasualReport(const FString& MatchId, const FString& Mode, const FString& Map,
	int32 DurationSec, int32 Elims, int32 Tags, int32 Objective, int32 Builder, bool bCompleted, bool bWon)
{
	if (!IsLoggedIn())
	{
		return;   // casual XP credits THIS account only — nothing to credit when logged out
	}
	// Proper JSON serialization (community-map labels are free text — quotes would corrupt a Printf body).
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("matchId"), MatchId);
	Root->SetStringField(TEXT("mode"), Mode);
	Root->SetStringField(TEXT("map"), Map);
	Root->SetNumberField(TEXT("durationSec"), DurationSec);
	const TSharedRef<FJsonObject> Stats = MakeShared<FJsonObject>();
	Stats->SetNumberField(TEXT("elims"), Elims);
	Stats->SetNumberField(TEXT("tags"), Tags);
	Stats->SetNumberField(TEXT("objective"), Objective);
	Stats->SetNumberField(TEXT("builder"), Builder);
	Stats->SetBoolField(TEXT("completed"), bCompleted);
	Stats->SetBoolField(TEXT("won"), bWon);
	Root->SetObjectField(TEXT("stats"), Stats);
	FString Body;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(Root, Writer);
	// Session-token auth (AuthMode 1) — the Worker resolves the account from the token and clamps/caps the XP.
	Request(TEXT("POST"), TEXT("/v1/casual-report"), Body, /*AuthMode=*/1,
		[](int32 Code, const FString& Resp)
		{
			UE_LOG(CombatForgeLog, Log, TEXT("Backend: casual-report -> %d %s"), Code, *Resp);
		});
}
