#include "SceneSelector_StreamingLevels.h"
#include "Containers/UnrealString.h"
#include "Engine/World.h"
#include "Engine/LevelStreaming.h"
#pragma optimize("", off)
static ULevelStreaming* findStreamingLevelByName(const UWorld& World, const FString& FindName)
{
    for (ULevelStreaming* streamingLevel : World.GetStreamingLevels())
    {
        FString LevelName = FPackageName::GetShortName(streamingLevel->GetWorldAssetPackageName());
        if (streamingLevel->GetWorld())
            LevelName.RemoveFromStart(streamingLevel->GetWorld()->StreamingLevelsPrefix);
        if (FindName == LevelName)
            return streamingLevel;
    }
    return nullptr;
}

bool SceneSelector_StreamingLevels::OnLoadedSchema(const UWorld& World, const RenderStreamLink::Schema& Schema)
{
    // If there's a persistent level with blueprints, include that in all scenes as common properties.
    AActor* persistentRoot = World.PersistentLevel->GetLevelScriptActor();

    m_specs.resize(Schema.scenes.nScenes);
    for (uint32_t i = 0; i < Schema.scenes.nScenes; ++i)
    {
        RenderStreamLink::RemoteParameters& parameters = Schema.scenes.scenes[i];
        const FString SceneName = UTF8_TO_TCHAR(parameters.name);
        ULevelStreaming* streamingLevel = findStreamingLevelByName(World, SceneName);

        SchemaSpec& spec = m_specs[i];
        spec.streamingLevel = streamingLevel;
        spec.persistentRoot = persistentRoot;
        spec.hash = parameters.hash;
        spec.nParameters = parameters.nParameters;

        if (!streamingLevel || streamingLevel->IsLevelLoaded())
        {
            spec.loaded = true;
            ValidateLevel(i);
        }
        else
        {
            UE_LOG(LogRenderStream, Log, TEXT("Skipping validation of unloaded streaming level %s"), UTF8_TO_TCHAR(Schema.scenes.scenes[i].name));
        }
    }

    return true;
}

void SceneSelector_StreamingLevels::ApplyScene(const UWorld& World, uint32_t sceneId)
{
    if (sceneId >= m_specs.size())
    {
        UE_LOG(LogRenderStream, Error, TEXT("Unable to get frame parameters - scene id %d >= %d"), sceneId, m_specs.size());
        return;
    }

    SchemaSpec& spec = m_specs[sceneId];
    if (!spec.loaded)
    {
        check(spec.streamingLevel != nullptr); // the persistent root scene 0 shouldn't be able to get here.

        if (!spec.streamingLevel->IsLevelLoaded())
        {
            UE_LOG(LogRenderStream, Log, TEXT("Loading level %s"), *spec.streamingLevel->GetWorldAssetPackageFName().ToString());
            FLatentActionInfo LatentInfo;
            UGameplayStatics::LoadStreamLevel(&World, spec.streamingLevel->GetWorldAssetPackageFName(), true, true, LatentInfo);
            return;
        }
        else
        {
            spec.loaded = true;
            ValidateLevel(sceneId);
        }
    }

    AActor* persistentRoot = World.PersistentLevel->GetLevelScriptActor();
    if (spec.streamingLevel == nullptr && spec.persistentRoot == persistentRoot) // base level
    {
        ApplyParameters(sceneId, { persistentRoot });

        for (ULevelStreaming* streamingLevel : World.GetStreamingLevels())
        {
            streamingLevel->SetShouldBeVisible(false);
        }
    }
    else
    {
        for (ULevelStreaming* streamingLevel : World.GetStreamingLevels())
        {
            if (spec.streamingLevel == streamingLevel)
            {
                if (!streamingLevel->IsLevelLoaded())
                {
                }
                else
                {
                    streamingLevel->SetShouldBeVisible(true);

                    ApplyParameters(sceneId, { spec.persistentRoot, streamingLevel->GetLevelScriptActor() });
                }
            }
            else if (spec.streamingLevel != nullptr)
            {
                streamingLevel->SetShouldBeVisible(false); // hide all levels not associated with this schema
            }
        }
    }
}

bool SceneSelector_StreamingLevels::ValidateLevel(uint32_t sceneId)
{
    RenderStreamLink::RemoteParameters& parameters = Schema().scenes.scenes[sceneId];
    const SchemaSpec& spec = m_specs[sceneId];
    UE_LOG(LogRenderStream, Log, TEXT("Validating schema for %s with %d parameters"), UTF8_TO_TCHAR(parameters.name), parameters.nParameters);
    AActor* levelRoot = spec.streamingLevel ? spec.streamingLevel->GetLevelScriptActor() : nullptr;
    if (!ValidateParameters(parameters, { spec.persistentRoot, levelRoot }))
    {
        UE_LOG(LogRenderStream, Error, TEXT("Failed to validate schema for %s"), UTF8_TO_TCHAR(parameters.name));
        return false;
    }

    return true;
}
#pragma optimize("", on)
