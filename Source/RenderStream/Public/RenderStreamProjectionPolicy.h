#pragma once

#include "RenderStreamLink.h"
#include "Render/Projection/IDisplayClusterProjectionPolicyFactory.h"
#include "Render/Projection/IDisplayClusterProjectionPolicy.h"
#include <deque>
#include <mutex>

class UCameraComponent;
class UWorld;
class FRenderStreamModule;

DECLARE_LOG_CATEGORY_EXTERN(LogRenderStreamPolicy, Log, All);

/**
 * 'renderstream' policy for disguise integration
 */
class FRenderStreamProjectionPolicy
    : public IDisplayClusterProjectionPolicy
{
public:
    FRenderStreamProjectionPolicy(const FString& ViewportId, const TMap<FString, FString>& Parameters);
    virtual ~FRenderStreamProjectionPolicy();

    //////////////////////////////////////////////////////////////////////////////////////////////
    // IDisplayClusterProjectionPolicy
    //////////////////////////////////////////////////////////////////////////////////////////////
    virtual void StartScene(UWorld* World) override;
    virtual void EndScene() override;
    virtual bool HandleAddViewport(const FIntPoint& ViewportSize, const uint32 ViewsAmount) override;
    virtual void HandleRemoveViewport() override;

    virtual bool CalculateView(const uint32 ViewIdx, FVector& InOutViewLocation, FRotator& InOutViewRotation, const FVector& ViewOffset, const float WorldToMeters, const float NCP, const float FCP) override;
    virtual bool GetProjectionMatrix(const uint32 ViewIdx, FMatrix& OutPrjMatrix) override;

    virtual bool IsWarpBlendSupported() override { return true; }
    virtual void ApplyWarpBlend_RenderThread(const uint32 ViewIdx, FRHICommandListImmediate& RHICmdList, FRHITexture2D* SrcTexture, const FIntRect& ViewportRect) override;

    const FString& GetViewportId() const { return ViewportId; }

    const ACameraActor* GetTemplateCamera() const;

    const TMap<FString, FString>& GetParameters() const
    {
        return Parameters;
    }

    void ApplyCameraData(const RenderStreamLink::FrameData& frameData, const RenderStreamLink::CameraData& cameraData);

    const int32_t GetPlayerControllerID() const { return PlayerControllerID; }

protected:
    const FString ViewportId;
    TMap<FString, FString> Parameters;

    // Near/far clip planes
    float NCP;
    float FCP;
    // Capture settings
    TWeakObjectPtr<ACameraActor> Camera = nullptr;
    TWeakObjectPtr<ACameraActor> Template = nullptr;
    TSharedPtr<FFrameStream> Stream = nullptr;
    int32_t PlayerControllerID = INDEX_NONE;

    FRenderStreamModule* Module;

    std::mutex m_frameResponsesLock;
    std::deque<RenderStreamLink::CameraResponseData> m_frameResponses;
};


/**
 * Implements projection policy factory for the 'renderstream' policy
 */
class FRenderStreamProjectionPolicyFactory
    : public IDisplayClusterProjectionPolicyFactory
{
public:
    FRenderStreamProjectionPolicyFactory();
    virtual ~FRenderStreamProjectionPolicyFactory();

public:
    //////////////////////////////////////////////////////////////////////////////////////////////
    // IDisplayClusterProjectionPolicyFactory
    //////////////////////////////////////////////////////////////////////////////////////////////
    virtual TSharedPtr<IDisplayClusterProjectionPolicy> Create(const FString& PolicyType, const FString& RHIName, const FString& ViewportId, const TMap<FString, FString>& Parameters) override;

    TArray<TSharedPtr<FRenderStreamProjectionPolicy>> GetPolicies();
    TSharedPtr<FRenderStreamProjectionPolicy>         GetPolicyByViewport(const FString& ViewportId);
    TSharedPtr<FRenderStreamProjectionPolicy>         GetPolicyBySceneViewFamily(int32 ViewFamilyIdx) const;

    static constexpr auto RenderStreamPolicyType = TEXT("renderstream");

private:
    TArray<TSharedPtr<FRenderStreamProjectionPolicy>> Policies;
};
