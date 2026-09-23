// Copyright Epic Games, Inc. All Rights Reserved.

#include "ScholaBridge.h"

#include "PursuitAI.h"

#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"
#include "Schola.h"

bool UPursuitAIScholaBridge::EnsureScholaLoaded()
{
	if (FModuleManager::Get().IsModuleLoaded(TEXT("Schola")))
	{
		return true;
	}

	// Typed load: this call compiling and linking is the actual proof that
	// PursuitAI resolved Schola's public headers and exported symbols.
	FModuleManager::LoadModuleChecked<FScholaModule>(TEXT("Schola"));
	return FModuleManager::Get().IsModuleLoaded(TEXT("Schola"));
}

FString UPursuitAIScholaBridge::GetScholaPluginVersion()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Schola"));
	if (!Plugin.IsValid())
	{
		return TEXT("unknown");
	}

	return Plugin->GetDescriptor().VersionName;
}

void UPursuitAIScholaBridge::LogScholaStatus()
{
	const bool bLoaded = EnsureScholaLoaded();

	UE_LOG(LogPursuitAI, Display,
		TEXT("[PursuitAI] schola_module_loaded=%s schola_plugin_version=%s"),
		bLoaded ? TEXT("true") : TEXT("false"),
		*GetScholaPluginVersion());
}

// Registered when this module loads. Run headless with:
//   UnrealEditor-Cmd.exe <project> -nullrhi -unattended -ExecCmds="PursuitAI.ScholaStatus"
static FAutoConsoleCommand GPursuitAIScholaStatusCommand(
	TEXT("PursuitAI.ScholaStatus"),
	TEXT("Log the PursuitAI <-> Schola link status (module loaded / plugin version)."),
	FConsoleCommandDelegate::CreateStatic(&UPursuitAIScholaBridge::LogScholaStatus));
