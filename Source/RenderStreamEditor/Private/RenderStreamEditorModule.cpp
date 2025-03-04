// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.


#include "RenderStreamEditorModule.h"
#include "Modules/ModuleManager.h"

#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "ObjectTools.h"
#include "PropertyEditorDelegates.h"
#include "PropertyEditorModule.h"
#include "UObject/UObjectBase.h"
#include "Engine/AssetManager.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelScriptActor.h"
#include "Engine/World.h"

#include "ISettingsModule.h"
#include "RenderStreamChannelCacheAsset.h"
#include "RenderStreamChannelDefinition.h"
#include "RenderStreamCustomization.h"
#include "RenderStreamSettings.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/ObjectLibrary.h"

#include "RenderStream/Public/RenderStreamLink.h"
#include <set>
#include <string>
#include <vector>

#include "GameMapsSettings.h"

DEFINE_LOG_CATEGORY(LogRenderStreamEditor);

#define LOCTEXT_NAMESPACE "RenderStreamEditor"

const FString CacheFolder = TEXT("/disguiseuerenderstream/Cache");

void FRenderStreamEditorModule::StartupModule()
{
    {
        auto& PropertyModule = FModuleManager::LoadModuleChecked< FPropertyEditorModule >("PropertyEditor");

        PropertyModule.RegisterCustomClassLayout(
            "RenderStreamChannelVisibility",
            FOnGetDetailCustomizationInstance::CreateStatic(&MakeVisibilityCustomizationInstance)
        );
        PropertyModule.RegisterCustomClassLayout(
            "RenderStreamChannelDefinition",
            FOnGetDetailCustomizationInstance::CreateStatic(&MakeDefinitionCustomizationInstance)
        );
        PropertyModule.RegisterCustomClassLayout(
            "RenderStreamSettings",
            FOnGetDetailCustomizationInstance::CreateStatic(&MakeSettingsCustomizationInstance)
        );
        

        PropertyModule.NotifyCustomizationModuleChanged();
    }

    void PreAssetDelete(const TArray<UObject*> InObjectToDelete);
    FEditorDelegates::PostSaveWorld.AddRaw(this, &FRenderStreamEditorModule::OnPostSaveWorld);
    FEditorDelegates::OnAssetsDeleted.AddRaw(this, &FRenderStreamEditorModule::OnAssetsDeleted);
    FCoreDelegates::OnBeginFrame.AddRaw(this, &FRenderStreamEditorModule::OnBeginFrame);
    FCoreDelegates::OnPostEngineInit.AddRaw(this, &FRenderStreamEditorModule::OnPostEngineInit);
}

void FRenderStreamEditorModule::ShutdownModule()
{
    if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
    {
        auto& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
        PropertyModule.UnregisterCustomClassLayout("RenderStreamChannelVisibility");
    }

    FEditorDelegates::PostSaveWorld.RemoveAll(this);
    FEditorDelegates::OnAssetsDeleted.RemoveAll(this);
    FCoreDelegates::OnBeginFrame.RemoveAll(this);
    FCoreDelegates::OnPostEngineInit.RemoveAll(this);
    if (GEditor)
        GEditor->OnBlueprintCompiled().RemoveAll(this);

    UnregisterSettings();
}

FString FRenderStreamEditorModule::StreamName()
{
    return FString(FApp::GetProjectName()) + "_Editor"; // TODO: to support editor this will have to not be like this
}

void FRenderStreamEditorModule::DeleteCaches(const TArray<FAssetData>& InCachesToDelete)
{
    TArray<UObject*> Objects;
    for (const FAssetData& Cache : InCachesToDelete)
    {
        UPackage* Package = Cast<UPackage>(Cache.GetAsset());
        if (Package)
        {
            UObject* Asset = Package->FindAssetInPackage();
            if (Asset)
            {
                Objects.Push(Package->FindAssetInPackage());
            }
        }
    }

    if (Objects.Num() > 0) // Actually stalls for ages even if empty.
        ObjectTools::ForceDeleteObjects(Objects, false);
}

void CreateField(FRenderStreamExposedParameterEntry& parameter, FString group, FString displayName_, FString suffix, FString key_, FString undecoratedSuffix, float min, float max, float step, float defaultValue, TArray<FString> options = {})
{
    FString key = key_ + (undecoratedSuffix.IsEmpty() ? "" : "_" + undecoratedSuffix);
    FString displayName = displayName_ + (suffix.IsEmpty() ? "" : " " + suffix);

    if (options.Num() > 0)
    {
        min = 0;
        max = options.Num() - 1;
        step = 1;
    }

    parameter.Group = group;
    parameter.DisplayName = displayName;
    parameter.Key = key;
    parameter.Min = min;
    parameter.Max = max;
    parameter.Step = step;
    parameter.DefaultValue = defaultValue;
    parameter.Options = options;
    parameter.DmxOffset = -1; // Auto
    parameter.DmxType = 2; // Dmx16BigEndian
}

static void ConvertFields(RenderStreamLink::RemoteParameter* outputIterator, const TArray<FRenderStreamExposedParameterEntry>& input)
{
    for (const FRenderStreamExposedParameterEntry& entry : input)
    {
        RenderStreamLink::RemoteParameter& parameter = *outputIterator++;

        parameter.group = _strdup(TCHAR_TO_UTF8(*entry.Group));
        parameter.displayName = _strdup(TCHAR_TO_UTF8(*entry.DisplayName));
        parameter.key = _strdup(TCHAR_TO_UTF8(*entry.Key));
        parameter.min = entry.Min;
        parameter.max = entry.Max;
        parameter.step = entry.Step;
        parameter.defaultValue = entry.DefaultValue;
        parameter.nOptions = uint32_t(entry.Options.Num());
        parameter.options = static_cast<const char**>(malloc(parameter.nOptions * sizeof(const char*)));
        for (size_t j = 0; j < parameter.nOptions; ++j)
        {
            parameter.options[j] = _strdup(TCHAR_TO_UTF8(*entry.Options[j]));
        }
        parameter.dmxOffset = -1; // Auto
        parameter.dmxType = 2; // Dmx16BigEndian
    }
}

TArray<FString> EnumOptions(const FNumericProperty* NumericProperty)
{
    TArray<FString> Options;
    if (!NumericProperty->IsEnum())
        return Options;

    const UEnum* Enum = NumericProperty->GetIntPropertyEnum();
    if (!Enum)
        return Options;

    const int64 Max = Enum->GetMaxEnumValue();
    for (int64 i = 0; i < Max; ++i)
        Options.Push(Enum->GetDisplayNameTextByIndex(i).ToString());
    return Options;
}

void GenerateParameters(TArray<FRenderStreamExposedParameterEntry>& Parameters, const AActor* Root)
{
    if (!Root)
        return;
    for (TFieldIterator<FProperty> PropIt(Root->GetClass(), EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
    {
        const FProperty* Property = *PropIt;
        const FString Name = Property->GetName();
        const FString Category = Property->GetMetaData("Category");
        if (!Property->HasAllPropertyFlags(CPF_Edit | CPF_BlueprintVisible) || Property->HasAllPropertyFlags(CPF_DisableEditOnInstance))
        {
            UE_LOG(LogRenderStreamEditor, Verbose, TEXT("Unexposed property: %s"), *Name);
        }
        else if (const FBoolProperty* BoolProperty = CastField<const FBoolProperty>(Property))
        {
            const bool v = BoolProperty->GetPropertyValue_InContainer(Root);
            UE_LOG(LogRenderStreamEditor, Log, TEXT("Exposed bool property: %s is %d"), *Name, v);
            CreateField(Parameters.Emplace_GetRef(), Category, Name, "", Name, "", 0.f, 1.f, 1.f, v ? 1.f : 0.f, { "Off", "On" });
        }
        else if (const FByteProperty* ByteProperty = CastField<const FByteProperty>(Property))
        {
            const uint8 v = ByteProperty->GetPropertyValue_InContainer(Root);
            TArray<FString> Options = EnumOptions(ByteProperty);
            UE_LOG(LogRenderStreamEditor, Log, TEXT("Exposed int property: %s is %d [%s]"), *Name, v, *FString::Join(Options, TEXT(",")));
            const bool HasLimits = Property->HasMetaData("ClampMin") && Property->HasMetaData("ClampMax");
            const float Min = HasLimits ? FCString::Atof(*Property->GetMetaData("ClampMin")) : 0;
            const float Max = HasLimits ? FCString::Atof(*Property->GetMetaData("ClampMax")) : 255;
            CreateField(Parameters.Emplace_GetRef(), Category, Name, "", Name, "", Min, Max, 1.f, float(v), Options);
        }
        else if (const FIntProperty* IntProperty = CastField<const FIntProperty>(Property))
        {
            const int32 v = IntProperty->GetPropertyValue_InContainer(Root);
            TArray<FString> Options = EnumOptions(IntProperty);
            UE_LOG(LogRenderStreamEditor, Log, TEXT("Exposed int property: %s is %d [%s]"), *Name, v, *FString::Join(Options, TEXT(",")));
            const bool HasLimits = Property->HasMetaData("ClampMin") && Property->HasMetaData("ClampMax");
            const float Min = HasLimits ? FCString::Atof(*Property->GetMetaData("ClampMin")) : -1000;
            const float Max = HasLimits ? FCString::Atof(*Property->GetMetaData("ClampMax")) : +1000;
            CreateField(Parameters.Emplace_GetRef(), Category, Name, "", Name, "", Min, Max, 1.f, float(v), Options);
        }
        else if (const FFloatProperty* FloatProperty = CastField<const FFloatProperty>(Property))
        {
            const float v = FloatProperty->GetPropertyValue_InContainer(Root);
            UE_LOG(LogRenderStreamEditor, Log, TEXT("Exposed float property: %s is %f"), *Name, v);
            const bool HasLimits = Property->HasMetaData("ClampMin") && Property->HasMetaData("ClampMax");
            const float Min = HasLimits ? FCString::Atof(*Property->GetMetaData("ClampMin")) : -1;
            const float Max = HasLimits ? FCString::Atof(*Property->GetMetaData("ClampMax")) : +1;
            CreateField(Parameters.Emplace_GetRef(), Category, Name, "", Name, "", Min, Max, 0.001f, v);
        }
        else if (const FStructProperty* StructProperty = CastField<const FStructProperty>(Property))
        {
            const void* StructAddress = StructProperty->ContainerPtrToValuePtr<void>(Root);
            if (StructProperty->Struct == TBaseStructure<FVector>::Get())
            {
                FVector v;
                StructProperty->CopyCompleteValue(&v, StructAddress);
                UE_LOG(LogRenderStreamEditor, Log, TEXT("Exposed vector property: %s is <%f, %f, %f>"), *Name, v.X, v.Y, v.Z);
                CreateField(Parameters.Emplace_GetRef(), Category, Name, "x", Name, "x", -1.f, +1.f, 0.001f, v.X);
                CreateField(Parameters.Emplace_GetRef(), Category, Name, "y", Name, "y", -1.f, +1.f, 0.001f, v.Y);
                CreateField(Parameters.Emplace_GetRef(), Category, Name, "z", Name, "z", -1.f, +1.f, 0.001f, v.Z);
            }
            else if (StructProperty->Struct == TBaseStructure<FColor>::Get())
            {
                FColor v;
                StructProperty->CopyCompleteValue(&v, StructAddress);
                UE_LOG(LogRenderStreamEditor, Log, TEXT("Exposed colour property: %s is <%d, %d, %d, %d>"), *Name, v.R, v.G, v.B, v.A);
                CreateField(Parameters.Emplace_GetRef(), Category, Name, "r", Name, "r", 0.f, 1.f, 0.0001f, v.R / 255.f);
                CreateField(Parameters.Emplace_GetRef(), Category, Name, "g", Name, "g", 0.f, 1.f, 0.0001f, v.G / 255.f);
                CreateField(Parameters.Emplace_GetRef(), Category, Name, "b", Name, "b", 0.f, 1.f, 0.0001f, v.B / 255.f);
                CreateField(Parameters.Emplace_GetRef(), Category, Name, "a", Name, "a", 0.f, 1.f, 0.0001f, v.A / 255.f);
            }
            else if (StructProperty->Struct == TBaseStructure<FLinearColor>::Get())
            {
                FLinearColor v;
                StructProperty->CopyCompleteValue(&v, StructAddress);
                UE_LOG(LogRenderStreamEditor, Log, TEXT("Exposed linear colour property: %s is <%f, %f, %f, %f>"), *Name, v.R, v.G, v.B, v.A);
                CreateField(Parameters.Emplace_GetRef(), Category, Name, "r", Name, "r", 0.f, 1.f, 0.0001f, v.R);
                CreateField(Parameters.Emplace_GetRef(), Category, Name, "g", Name, "g", 0.f, 1.f, 0.0001f, v.G);
                CreateField(Parameters.Emplace_GetRef(), Category, Name, "b", Name, "b", 0.f, 1.f, 0.0001f, v.B);
                CreateField(Parameters.Emplace_GetRef(), Category, Name, "a", Name, "a", 0.f, 1.f, 0.0001f, v.A);
            }
            else
            {
                UE_LOG(LogRenderStreamEditor, Log, TEXT("Exposed struct property: %s"), *Name);
            }
        }
        else
        {
            UE_LOG(LogRenderStreamEditor, Log, TEXT("Unsupported exposed property: %s"), *Name);
        }
    }
}

void GenerateScene(RenderStreamLink::RemoteParameters& SceneParameters, const URenderStreamChannelCacheAsset* cache, const URenderStreamChannelCacheAsset* persistent)
{
    FString sceneName = FPackageName::GetShortName(cache->Level.GetAssetPathName());
    SceneParameters.name = _strdup(TCHAR_TO_UTF8(*sceneName));

    const uint32_t nParams = (persistent ? persistent->ExposedParams.Num() : 0) + cache->ExposedParams.Num();
    SceneParameters.nParameters = nParams;
    SceneParameters.parameters = static_cast<RenderStreamLink::RemoteParameter*>(malloc(nParams * sizeof(RenderStreamLink::RemoteParameter)));

    size_t offset = 0;
    if (persistent)
    {
        ConvertFields(SceneParameters.parameters, persistent->ExposedParams);
        offset += persistent->ExposedParams.Num();
    }
    ConvertFields(SceneParameters.parameters + offset, cache->ExposedParams);

    UE_LOG(LogRenderStreamEditor, Log, TEXT("Generated schema for scene: %s"), UTF8_TO_TCHAR(SceneParameters.name));
}

bool TryGetCache(const FString LevelPath, URenderStreamChannelCacheAsset*& Cache)
{
    const FString PathName = CacheFolder + LevelPath;
    const FSoftObjectPath Path(PathName);
    Cache = Cast<URenderStreamChannelCacheAsset>(Path.TryLoad());
    return Cache != nullptr;
}

URenderStreamChannelCacheAsset* GetOrCreateCache(ULevel* Level)
{
    URenderStreamChannelCacheAsset* Cache = nullptr;
    const FString LevelPath = Level->GetPackage()->GetPathName();
    if (!TryGetCache(LevelPath, Cache)) // Asset doesn't exists.
    {
        const FString PathName = CacheFolder + LevelPath;
        FString AssetName;
        if (!PathName.Split(TEXT("/"), nullptr, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
        {
            AssetName = PathName;
        }

        UPackage* Package = FindPackage(nullptr, *PathName);
        if (!Package)
        {
            Package = CreatePackage(*PathName);
        }

        Package->FullyLoad();
        Cache = NewObject<URenderStreamChannelCacheAsset>(Package, FName(AssetName), RF_Public | RF_Standalone);
    }

    return Cache;
}

URenderStreamChannelCacheAsset* UpdateLevelChannelCache(ULevel* Level)
{
    URenderStreamChannelCacheAsset* Cache = GetOrCreateCache(Level);

    // Update the Cache.
    const FString LevelPath = Level->GetPackage()->GetPathName();
    Cache->Level = LevelPath;
    Cache->Channels.Empty();
    for (auto Actor : Level->Actors)
    {
        if (Actor)
        {
            const URenderStreamChannelDefinition* Definition = Actor->FindComponentByClass<URenderStreamChannelDefinition>();
            if (Definition)
            {
                Cache->Channels.Emplace(TCHAR_TO_UTF8(*Actor->GetName()));
            }
        }
    }

    Cache->ExposedParams.Empty();
    GenerateParameters(Cache->ExposedParams, Level->GetLevelScriptActor());

    Cache->SubLevels.Empty();
    for (ULevelStreaming* SubLevel : Level->GetWorld()->GetStreamingLevels())
        Cache->SubLevels.Add(SubLevel->GetWorldAsset()->GetPackage()->GetPathName());

    // Save the Cache.
    UPackage* Package = Cache->GetPackage();
    Package->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(Cache);
    const FString PackageFileName = FPackageName::LongPackageNameToFilename(CacheFolder + LevelPath, FPackageName::GetAssetPackageExtension());
    bool bSaved = UPackage::SavePackage(
        Package,
        Cache,
        EObjectFlags::RF_Public | EObjectFlags::RF_Standalone,
        *PackageFileName,
        GError,
        nullptr,
        true,
        true,
        SAVE_NoError
    );

    return Cache;
}

void UpdateChannelCache()
{
    UWorld* World = GEditor->GetEditorWorldContext().World();
    for (ULevel* Level : World->GetLevels())
    {
        if (Level)
            UpdateLevelChannelCache(Level);
    }

    for (const ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
    {
        if (StreamingLevel->IsLevelLoaded())
            UpdateLevelChannelCache(StreamingLevel->GetLoadedLevel());
    }

    // Loop over all levels and make sure caches exist for them.
    TArray<FAssetData> LevelAssets;
    const auto LevelLibrary = UObjectLibrary::CreateLibrary(ULevel::StaticClass(), false, true);
    LevelLibrary->LoadAssetDataFromPath("/Game/");
    LevelLibrary->GetAssetDataList(LevelAssets);
    for (FAssetData const& Asset : LevelAssets)
    {
        // Create the required caches if they don't exist.
        URenderStreamChannelCacheAsset* Cache;
        if (!TryGetCache(CacheFolder + Asset.GetFullName(), Cache))
            Cache = UpdateLevelChannelCache(Cast<ULevel>(Asset.FastGetAsset(true)));
    }
}

URenderStreamChannelCacheAsset* GetDefaultMapCache()
{
    const FString DefaultMap = UGameMapsSettings::GetGameDefaultMap();
    URenderStreamChannelCacheAsset* Cache = nullptr;
    TryGetCache(DefaultMap, Cache);
    // This should never be the case because we will have already generated all the caches for the levels previously.
    if (!Cache)
    {
        const FSoftObjectPath Path(DefaultMap);
        ULevel* Level = Cast<ULevel>(Path.TryLoad());
        if (Level)
            Cache = UpdateLevelChannelCache(Level);
    }

    return Cache;
}

void FRenderStreamEditorModule::GenerateAssetMetadata()
{
    if (!RenderStreamLink::instance().isAvailable())
    {
        UE_LOG(LogRenderStreamEditor, Warning, TEXT("RenderStreamLink unavailable, skipped GenerateAssetMetadata"));
        return;
    }

    const URenderStreamSettings* settings = GetDefault<URenderStreamSettings>();

    // Update currently loaded levels
    UpdateChannelCache();

    TArray<URenderStreamChannelCacheAsset*> ChannelCaches;
    auto ObjectLibrary = UObjectLibrary::CreateLibrary(URenderStreamChannelCacheAsset::StaticClass(), false, false);
    ObjectLibrary->LoadAssetsFromPath(CacheFolder);
    ObjectLibrary->GetObjects(ChannelCaches);

    TArray<FAssetData> CachesForDelete;
    std::set<std::string> Channels;
    TMap<FSoftObjectPath, URenderStreamChannelCacheAsset*> LevelParams;
    for (size_t i = 0; i < ChannelCaches.Num(); ++i)
    {
        URenderStreamChannelCacheAsset* Cache = ChannelCaches[i];
        const FName PathName = Cache->Level.GetAssetPathName();
        if (FPackageName::DoesPackageExist(PathName.ToString()))
        {
            for (const FString& Channel : Cache->Channels)
            {
                Channels.emplace(TCHAR_TO_ANSI(*Channel));
            }

            LevelParams.Add(Cache->Level) = Cache;
        }
        else
        {
            // Remove them so we don't process deleted caches.
            CachesForDelete.Add(Cache->GetPackage());
            ChannelCaches.RemoveAt(i);
            --i;
        }
    }

    RenderStreamLink::ScopedSchema Schema;
    Schema.schema.channels.nChannels = uint32_t(Channels.size());
    Schema.schema.channels.channels = static_cast<const char**>(malloc(Schema.schema.channels.nChannels * sizeof(const char*)));
    auto It = Channels.begin();
    for (size_t i = 0; i < Schema.schema.channels.nChannels && It != Channels.end(); ++i, ++It)
        Schema.schema.channels.channels[i] = _strdup(It->c_str());

    UWorld* World = GEditor->GetEditorWorldContext().World();

    switch (settings->SceneSelector)
    {
    case ERenderStreamSceneSelector::None:
    {
        URenderStreamChannelCacheAsset* MainMap = GetDefaultMapCache();
        if (MainMap)
        {
            Schema.schema.scenes.nScenes = 1;
            Schema.schema.scenes.scenes = static_cast<RenderStreamLink::RemoteParameters*>(malloc(Schema.schema.scenes.nScenes * sizeof(RenderStreamLink::RemoteParameters)));
            GenerateScene(*Schema.schema.scenes.scenes, MainMap, nullptr);
        }
        else
            UE_LOG(LogRenderStreamEditor, Error, TEXT("No default map defined, either use Maps scene selector or define a default map."));

        break;
    }

    case ERenderStreamSceneSelector::StreamingLevels:
    {
        URenderStreamChannelCacheAsset* MainMap = GetDefaultMapCache();
        if (MainMap)
        {
            Schema.schema.scenes.nScenes = 1 + MainMap->SubLevels.Num();
            Schema.schema.scenes.scenes = static_cast<RenderStreamLink::RemoteParameters*>(malloc(Schema.schema.scenes.nScenes * sizeof(RenderStreamLink::RemoteParameters)));
            RenderStreamLink::RemoteParameters* SceneParameters = Schema.schema.scenes.scenes;

            GenerateScene(*SceneParameters++, MainMap, nullptr);
            for (FSoftObjectPath Path : MainMap->SubLevels)
                GenerateScene(*SceneParameters++, LevelParams[Path], MainMap);
        }
        else
            UE_LOG(LogRenderStreamEditor, Error, TEXT("No default map defined, either use Maps scene selector or define a default map."));

        break;
    }

    case ERenderStreamSceneSelector::Maps:
    {
        TMap<const URenderStreamChannelCacheAsset*, const URenderStreamChannelCacheAsset*> LevelParents;
        for (const URenderStreamChannelCacheAsset* Cache : ChannelCaches)
        {
            for (FSoftObjectPath Path : Cache->SubLevels)
                LevelParents.Add(LevelParams[Path], Cache);
        }

        Schema.schema.scenes.nScenes = ChannelCaches.Num();
        Schema.schema.scenes.scenes = static_cast<RenderStreamLink::RemoteParameters*>(malloc(Schema.schema.scenes.nScenes * sizeof(RenderStreamLink::RemoteParameters)));
        RenderStreamLink::RemoteParameters* SceneParameters = Schema.schema.scenes.scenes;
        
        for (const URenderStreamChannelCacheAsset* Cache : ChannelCaches)
        {
            const URenderStreamChannelCacheAsset** Entry = LevelParents.Find(Cache);
            GenerateScene(*SceneParameters++, Cache, Entry != nullptr ? *Entry : nullptr);
        }

        break;
    }
    }

    if (RenderStreamLink::instance().rs_saveSchema(TCHAR_TO_UTF8(*FPaths::GetProjectFilePath()), &Schema.schema) != RenderStreamLink::RS_ERROR_SUCCESS)
    {
        UE_LOG(LogRenderStreamEditor, Error, TEXT("Failed to save schema"));
    }

    ObjectLibrary->ClearLoaded();
    DeleteCaches(CachesForDelete);
}

void FRenderStreamEditorModule::OnPostSaveWorld(uint32, UWorld* World, bool bSuccess)
{
    if (bSuccess)
        DirtyAssetMetadata = true;
}

void FRenderStreamEditorModule::OnAssetsDeleted(const TArray<UClass*>& DeletedAssetClasses)
{
    if (DeletedAssetClasses.Contains(UWorld::StaticClass()))
        DirtyAssetMetadata = true;
}

void FRenderStreamEditorModule::OnBeginFrame()
{
    // We have to generate the metadata here because renaming a level does not trigger assets deleted
    // and the old level is still around when OnPostSaveWorld is triggered, remove this once fixed by Epic.
    if (DirtyAssetMetadata)
    {
        GenerateAssetMetadata();
        DirtyAssetMetadata = false;
    }
}

void FRenderStreamEditorModule::OnPostEngineInit()
{
    RegisterSettings();
}

void FRenderStreamEditorModule::RegisterSettings()
{
    if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
    {
        SettingsModule->RegisterSettings("Project", "Plugins", "DisguiseRenderStream",
            LOCTEXT("RuntimeSettingsName", "Disguise RenderStream"),
            LOCTEXT("RuntimeSettingsDescription", "Project settings for Disguise RenderStream plugin"),
            GetMutableDefault<URenderStreamSettings>()
        );
    }
}

void FRenderStreamEditorModule::UnregisterSettings()
{
    if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
    {
        SettingsModule->UnregisterSettings("Project", "Plugins", "DisguiseRenderStream");
    }
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FRenderStreamEditorModule, RenderStreamEditor);
