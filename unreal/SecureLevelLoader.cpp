#include "SecureLevelLoader.h"
#include "Engine/World.h"

void USecureLevelLoader::LoadLevelBundle(const FString& GameId, const FString& ManifestUrl)
{
    RequestManifest(ManifestUrl);
}

void USecureLevelLoader::RequestManifest(const FString& ManifestUrl)
{
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetVerb(TEXT("GET"));
    Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
    Request->SetHeader(TEXT("User-Agent"), TEXT("PlayStationConsoleClient/1.0"));
    Request->SetURL(ManifestUrl);

    Request->OnProcessRequestComplete().BindUObject(this, &USecureLevelLoader::OnManifestResponse);
    Request->ProcessRequest();
}

void USecureLevelLoader::OnManifestResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully)
{
    if (!bConnectedSuccessfully || !Response.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("Manifest request failed."));
        return;
    }

    if (Response->GetResponseCode() != 200)
    {
        UE_LOG(LogTemp, Warning, TEXT("Manifest HTTP error: %d"), Response->GetResponseCode());
        return;
    }

    FString Body = Response->GetContentAsString();
    TSharedPtr<FJsonObject> JsonRoot;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);

    if (!FJsonSerializer::Deserialize(Reader, JsonRoot) || !JsonRoot.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("Manifest JSON parse failed."));
        return;
    }

    if (!ValidateManifestSchema(JsonRoot))
    {
        UE_LOG(LogTemp, Warning, TEXT("Manifest schema validation failed."));
        return;
    }

    AsyncTask(ENamedThreads::GameThread, [this, JsonRoot]()
    {
        SpawnActorsFromManifest(JsonRoot);
    });
}

bool USecureLevelLoader::ValidateManifestSchema(const TSharedPtr<FJsonObject>& ManifestRoot) const
{
    if (!ManifestRoot->HasField(TEXT("gameId")) || !ManifestRoot->HasField(TEXT("objects")))
    {
        return false;
    }

    const FString GameId = ManifestRoot->GetStringField(TEXT("gameId"));
    if (GameId.IsEmpty())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Objects = nullptr;
    if (!ManifestRoot->TryGetArrayField(TEXT("objects"), Objects))
    {
        return false;
    }

    for (const TSharedPtr<FJsonValue>& Value : *Objects)
    {
        const TSharedPtr<FJsonObject> Obj = Value->AsObject();
        if (!Obj.IsValid())
        {
            return false;
        }

        if (!Obj->HasField(TEXT("id")) || !Obj->HasField(TEXT("type")) || !Obj->HasField(TEXT("location")))
        {
            return false;
        }

        const FString Type = Obj->GetStringField(TEXT("type"));
        if (!ApprovedAssets.Contains(Type))
        {
            return false;
        }
    }

    return true;
}

bool USecureLevelLoader::IsApprovedAssetPath(const FString& AssetPath) const
{
    for (const auto& Pair : ApprovedAssets)
    {
        if (AssetPath == Pair.Value)
        {
            return true;
        }
    }

    return false;
}

void USecureLevelLoader::SpawnActorsFromManifest(const TSharedPtr<FJsonObject>& ManifestRoot)
{
    UWorld* World = GEngine->GetCurrentPlayWorld();
    if (!World)
    {
        UE_LOG(LogTemp, Warning, TEXT("No active world."));
        return;
    }

    const TArray<TSharedPtr<FJsonValue>>* Objects = nullptr;
    if (!ManifestRoot->TryGetArrayField(TEXT("objects"), Objects))
    {
        return;
    }

    for (const TSharedPtr<FJsonValue>& Value : *Objects)
    {
        const TSharedPtr<FJsonObject> Obj = Value->AsObject();
        if (!Obj.IsValid())
        {
            continue;
        }

        const FString Type = Obj->GetStringField(TEXT("type"));
        const FString AssetPath = ApprovedAssets.FindRef(Type);

        if (AssetPath.IsEmpty() || !IsApprovedAssetPath(AssetPath))
        {
            UE_LOG(LogTemp, Warning, TEXT("Blocked unapproved type: %s"), *Type);
            continue;
        }

        UStaticMesh* ApprovedMesh = LoadObject<UStaticMesh>(nullptr, *AssetPath);
        if (!ApprovedMesh)
        {
            UE_LOG(LogTemp, Warning, TEXT("Failed to load approved asset: %s"), *AssetPath);
            continue;
        }

        const TArray<TSharedPtr<FJsonValue>>* LocationJson = nullptr;
        if (!Obj->TryGetArrayField(TEXT("location"), LocationJson) || LocationJson->Num() < 3)
        {
            continue;
        }

        float X = (*LocationJson)[0]->AsNumber();
        float Y = (*LocationJson)[1]->AsNumber();
        float Z = (*LocationJson)[2]->AsNumber();

        FTransform SpawnTransform;
        SpawnTransform.SetLocation(FVector(X, Y, Z));
        SpawnTransform.SetRotation(FQuat::Identity);

        AActor* SpawnedActor = World->SpawnActor<AActor>(AActor::StaticClass(), SpawnTransform);
        if (!SpawnedActor)
        {
            continue;
        }

        UStaticMeshComponent* MeshComp = NewObject<UStaticMeshComponent>(SpawnedActor);
        MeshComp->SetStaticMesh(ApprovedMesh);
        MeshComp->SetMobility(EComponentMobility::Movable);
        MeshComp->RegisterComponent();
        SpawnedActor->AddInstanceComponent(MeshComp);

        SpawnedActor->Tags.Add(*Obj->GetStringField(TEXT("id")));
    }
}
