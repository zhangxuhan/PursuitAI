// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "ScholaBridge.generated.h"

/**
 * Diagnostic bridge between this project and the Schola plugin.
 *
 * Round 2 only proves the link is real: the module compiles against Schola's
 * public headers, the runtime module loads, and the plugin descriptor is
 * reachable. Agent / trainer plumbing is added in later rounds.
 */
UCLASS()
class PURSUITAI_API UPursuitAIScholaBridge : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Loads the Schola runtime module on demand and reports whether it ended up loaded.
	 * A successful call means this module resolved Schola's headers and exports.
	 */
	UFUNCTION(BlueprintCallable, Category = "PursuitAI|Schola")
	static bool EnsureScholaLoaded();

	/** VersionName from Schola.uplugin, or "unknown" when the descriptor cannot be read. */
	UFUNCTION(BlueprintCallable, Category = "PursuitAI|Schola")
	static FString GetScholaPluginVersion();

	/** Writes the whole link status into the log. Also exposed as `PursuitAI.ScholaStatus`. */
	static void LogScholaStatus();
};
