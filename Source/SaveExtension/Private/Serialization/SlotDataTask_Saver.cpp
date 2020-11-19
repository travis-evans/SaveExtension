// Copyright 2015-2020 Piperift. All Rights Reserved.

#include "Serialization/SlotDataTask_Saver.h"

#include <GameFramework/GameModeBase.h>
#include <Serialization/MemoryWriter.h>

#include "SaveManager.h"
#include "SlotInfo.h"
#include "SlotData.h"
#include "SavePreset.h"
#include "FileAdapter.h"


/////////////////////////////////////////////////////
// USaveDataTask_Saver

void USlotDataTask_Saver::OnStart()
{
	USaveManager* Manager = GetManager();
	Manager->TryInstantiateInfo();

	bool bSave = true;
	const FString SlotNameStr = SlotName.ToString();
	//Overriding
	{
		const bool bFileExists = FFileAdapter::DoesFileExist(SlotNameStr);
		if (bOverride)
		{
			// Delete previous save
			if (bFileExists)
			{
				FFileAdapter::DeleteFile(SlotNameStr);
			}
		}
		else
		{
			//Only save if previous files don't exist
			//We don't want to serialize since it won't be saved anyway
			bSave = !bFileExists;
		}
	}

	if (bSave)
	{
		const UWorld* World = GetWorld();

		GetManager()->OnSaveBegan(Filter);

		SlotInfo = Manager->GetCurrentInfo();
		SlotData = Manager->GetCurrentData();
		SlotData->Clean(true);


		check(SlotInfo && SlotData);

		const bool bSlotWasDifferent = SlotInfo->FileName != SlotName;
		SlotInfo->FileName = SlotName;

		if (bSaveThumbnail)
		{
			SlotInfo->CaptureThumbnail(Width, Height);
		}

		// Time stats
		{
			SlotInfo->SaveDate = FDateTime::Now();

			// If this info has been loaded ever
			const bool bWasLoaded = SlotInfo->LoadDate.GetTicks() > 0;
			if (bWasLoaded)
			{
				// Now - Loaded
				const FTimespan SessionTime = SlotInfo->SaveDate - SlotInfo->LoadDate;

				SlotInfo->PlayedTime += SessionTime;

				if (!bSlotWasDifferent)
					SlotInfo->SlotPlayedTime += SessionTime;
				else
					SlotInfo->SlotPlayedTime = SessionTime;
			}
			else
			{
				// Slot is new, played time is world seconds
				SlotInfo->PlayedTime = FTimespan::FromSeconds(World->TimeSeconds);
				SlotInfo->SlotPlayedTime = SlotInfo->PlayedTime;
			}

			// Save current game seconds
			SlotData->TimeSeconds = World->TimeSeconds;
		}

		//Save Level info in both files
		// TODO: Only if map is an asset, otherway it must be empty
		SlotInfo->Map = FName{ World->GetMapName() };
		SlotData->Map = SlotData->Map;

		SerializeSync();
		SaveFile();
		return;
	}
	Finish(false);
}

void USlotDataTask_Saver::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (SaveTask && SaveTask->IsDone())
	{
		if (bSaveThumbnail)
		{
			if (SlotInfo && SlotInfo->GetThumbnail())
			{
				Finish(true);
			}
		}
		else
		{
			Finish(true);
		}
	}
}

void USlotDataTask_Saver::OnFinish(bool bSuccess)
{
	if (bSuccess)
	{
		// Clean serialization data
		SlotData->Clean(true);

		SELog(Preset, "Finished Saving", FColor::Green);
	}

	// Execute delegates
	USaveManager* Manager = GetManager();
	check(Manager);
	Delegate.ExecuteIfBound((Manager && bSuccess)? Manager->GetCurrentInfo() : nullptr);
	Manager->OnSaveFinished(Filter, !bSuccess);
}

void USlotDataTask_Saver::BeginDestroy()
{
	if (SaveTask)
	{
		SaveTask->EnsureCompletion(false);
		delete SaveTask;
	}

	Super::BeginDestroy();
}

void USlotDataTask_Saver::SerializeSync()
{
	// Has Authority
	if (GetWorld()->GetAuthGameMode())
	{
		// Save World
		SerializeWorld();
	}
}

void USlotDataTask_Saver::SerializeWorld()
{
	const UWorld* World = GetWorld();

	SELog(Preset, "World '" + World->GetName() + "'", FColor::Green, false, 1);

	const TArray<ULevelStreaming*>& Levels = World->GetStreamingLevels();

	// Threads available + 1 (Synchronous Thread)
	const int32 NumberOfThreads = FMath::Max(1, FPlatformMisc::NumberOfWorkerThreadsToSpawn() + 1);
	const int32 TasksPerLevel = FMath::Max(1, FMath::RoundToInt(float(NumberOfThreads) / (Levels.Num() + 1)));

	Tasks.Reserve(NumberOfThreads);

	SerializeLevelSync(World->GetCurrentLevel(), TasksPerLevel);
	for (const ULevelStreaming* Level : Levels)
	{
		if (Level->IsLevelLoaded())
		{
			SerializeLevelSync(Level->GetLoadedLevel(), TasksPerLevel, Level);
		}
	}

	RunScheduledTasks();
}

void USlotDataTask_Saver::SerializeLevelSync(const ULevel* Level, int32 AssignedTasks, const ULevelStreaming* StreamingLevel)
{
	check(IsValid(Level));

	if (!Preset->IsMTSerializationSave())
		AssignedTasks = 1;

	const FName LevelName = StreamingLevel ? StreamingLevel->GetWorldAssetPackageFName() : FPersistentLevelRecord::PersistentName;
	SELog(Preset, "Level '" + LevelName.ToString() + "'", FColor::Green, false, 1);


	// Find level record. By default, main level
	FLevelRecord* LevelRecord = &SlotData->MainLevel;
	if (StreamingLevel)
	{
		// Find or create the sub-level
		const int32 Index = SlotData->SubLevels.AddUnique({ StreamingLevel });
		LevelRecord = &SlotData->SubLevels[Index];
	}
	check(LevelRecord);

	// Empty level record before serializing it
	LevelRecord->Clean();

	Filter.BakeAllowedClasses();

	const int32 MinObjectsPerTask = 40;
	const int32 ActorCount = Level->Actors.Num();
	const int32 NumBalancedPerTask = FMath::CeilToInt((float) ActorCount / AssignedTasks);
	const int32 NumPerTask = FMath::Max(NumBalancedPerTask, MinObjectsPerTask);


	// Split all actors between multi-threaded tasks
	int32 Index = 0;
	while (Index < ActorCount)
	{
		const int32 NumRemaining = ActorCount - Index;
		const int32 NumToSerialize = FMath::Min(NumRemaining, NumPerTask);

		// Add new Task
		Tasks.Emplace(FMTTask_SerializeActors{
			GetWorld(), SlotData, &Level->Actors, Index, NumToSerialize, LevelRecord, Filter});

		Index += NumToSerialize;
	}
}

void USlotDataTask_Saver::RunScheduledTasks()
{
	// Start all serialization tasks
	if (Tasks.Num() > 0)
	{
		for (int32 I = 1; I < Tasks.Num(); ++I)
		{
			if (Preset->IsMTSerializationSave())
				Tasks[I].StartBackgroundTask();
			else
				Tasks[I].StartSynchronousTask();
		}
		// First task stores
		Tasks[0].StartSynchronousTask();
	}
	// Wait until all tasks have finished
	for (auto& AsyncTask : Tasks)
	{
		AsyncTask.EnsureCompletion();
	}
	// All tasks finished, sync data
	for (auto& AsyncTask : Tasks)
	{
		AsyncTask.GetTask().DumpData();
	}
	Tasks.Empty();
}

void USlotDataTask_Saver::SaveFile()
{
	USaveManager* Manager = GetManager();

	SaveTask = new FAsyncTask<FSaveFileTask>(
		Manager->GetCurrentInfo(), Manager->GetCurrentData(),
		SlotName.ToString(), Preset->bUseCompression);

	if (Preset->IsMTFilesSave())
	{
		SaveTask->StartBackgroundTask();
	}
	else
	{
		SaveTask->StartSynchronousTask();

		if (!bSaveThumbnail)
		{
			Finish(true);
		}
	}
}
