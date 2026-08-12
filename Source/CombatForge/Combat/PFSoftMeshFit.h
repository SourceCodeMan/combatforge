// Copyright (c) 2026 Tom Chapman. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/SoftObjectPath.h"

/**
 * First-success static-mesh TryLoad + two bounds-fit modes used by planted bomb,
 * bomb pickup, and ammo barrel. Path lists and target sizes stay at the call site.
 *
 * CenterToSize: uniform scale so max AABB dim == TargetSizeUU, origin at component 0.
 * SitOnGroundByHeight: uniform scale so height (extent.Z*2) == TargetHeightUU, bottom at Z=0.
 */

inline UStaticMesh* PFTryLoadStaticMesh(const TCHAR* Path)
{
	if (!Path || !*Path)
	{
		return nullptr;
	}
	return Cast<UStaticMesh>(FSoftObjectPath(Path).TryLoad());
}

inline UStaticMesh* PFTryLoadStaticMesh(const TCHAR* const* Paths, int32 Count, const TCHAR** OutPath = nullptr)
{
	if (OutPath)
	{
		*OutPath = nullptr;
	}
	if (!Paths)
	{
		return nullptr;
	}
	for (int32 i = 0; i < Count; ++i)
	{
		if (UStaticMesh* M = PFTryLoadStaticMesh(Paths[i]))
		{
			if (OutPath)
			{
				*OutPath = Paths[i];
			}
			return M;
		}
	}
	return nullptr;
}

template <int32 N>
inline UStaticMesh* PFTryLoadStaticMesh(const TCHAR* const (&Paths)[N], const TCHAR** OutPath = nullptr)
{
	return PFTryLoadStaticMesh(Paths, N, OutPath);
}

/** Uniform scale to TargetSizeUU on the tallest axis; recenter origin on the component. Returns the scale. */
inline float PFFitMeshCenterToSize(UStaticMesh* MeshAsset, UStaticMeshComponent* Comp, float TargetSizeUU)
{
	if (!MeshAsset || !Comp)
	{
		return 1.f;
	}
	const FBoxSphereBounds B = MeshAsset->GetBounds();
	const float MaxDim = FMath::Max3(B.BoxExtent.X, B.BoxExtent.Y, B.BoxExtent.Z) * 2.f;
	const float Sc = TargetSizeUU / FMath::Max(MaxDim, 1.f);
	Comp->SetRelativeScale3D(FVector(Sc));
	Comp->SetRelativeLocation(FVector(-B.Origin.X * Sc, -B.Origin.Y * Sc, -B.Origin.Z * Sc));
	return Sc;
}

/** Uniform scale to TargetHeightUU on height; sit the AABB bottom on Z=0. Returns the scale. */
inline float PFFitMeshSitOnGroundByHeight(UStaticMesh* MeshAsset, UStaticMeshComponent* Comp, float TargetHeightUU)
{
	if (!MeshAsset || !Comp)
	{
		return 1.f;
	}
	const FBoxSphereBounds B = MeshAsset->GetBounds();
	const float H = FMath::Max(B.BoxExtent.Z * 2.f, 1.f);
	const float Sc = TargetHeightUU / H;
	Comp->SetRelativeScale3D(FVector(Sc));
	Comp->SetRelativeLocation(FVector(
		-B.Origin.X * Sc,
		-B.Origin.Y * Sc,
		-(B.Origin.Z - B.BoxExtent.Z) * Sc));
	return Sc;
}
