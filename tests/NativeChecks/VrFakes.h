// Fake OpenVR interfaces for the offline VR frame-generation check.
//
// The module hooks IVRCompositor_029 and reads IVRSystem_023 / IVRSettings_003, so the harness supplies
// concrete implementations whose vtable entries can be detoured like the real runtime's. The compositor
// models a 90 Hz display clock shared by the game thread and the module's presenter thread, returns an
// analytically rotating HMD pose from the system, and records every submit with the pose it carried.
#pragma once

#include <openvr.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

// The four detoured compsitor methods must never be inlined into the harness: a devirtualized inline
// copy would bypass the detour the module installed on the function body.
#if defined(_MSC_VER)
#define BIGSCALE_CHECK_NOINLINE __declspec(noinline)
#else
#define BIGSCALE_CHECK_NOINLINE
#endif

#pragma warning(push)
// The generated interface stubs intentionally ignore their parameters.
#pragma warning(disable : 4100)

namespace bigscale_vrfg_check
{
// The interface declarations use short vr:: names; bringing the namespace in keeps every generated
// override identical to the header it implements.
using namespace vr;

// Running 90 Hz display clock. Both the simulated game thread and the presenter wait on it, so the
// module's virtual WaitGetPoses paces the game instead of a test-side sleep.
class DisplayClock
{
public:
    static constexpr double Period = 1.0 / 90.0;

    double Now() const
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }

    void WaitNextBoundary()
    {
        const double now = Now();
        const double next = (std::floor(now / Period) + 1.0) * Period;
        if (next <= now) return;
        // The Windows timer tick is 15.6 ms unless a process raises it, which would quantize an
        // 11.1 ms display frame. Sleep to 2 ms before the boundary, then spin the remainder.
        const double coarse = next - now - 0.002;
        if (coarse > 0.0) std::this_thread::sleep_for(std::chrono::duration<double>(coarse));
        while (Now() < next) std::this_thread::yield();
    }

private:
    const std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

// Rotation quaternion of a pose. The 3x3 block is a standard (column-vector) rotation matrix.
inline std::array<double, 4> RotationQuaternion(const HmdMatrix34_t& m)
{
    const double m00 = m.m[0][0], m01 = m.m[0][1], m02 = m.m[0][2];
    const double m10 = m.m[1][0], m11 = m.m[1][1], m12 = m.m[1][2];
    const double m20 = m.m[2][0], m21 = m.m[2][1], m22 = m.m[2][2];
    const double trace = m00 + m11 + m22;
    double x, y, z, w;
    if (trace > 0.0)
    {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        w = 0.25 * s; x = (m21 - m12) / s; y = (m02 - m20) / s; z = (m10 - m01) / s;
    }
    else if (m00 > m11 && m00 > m22)
    {
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        w = (m21 - m12) / s; x = 0.25 * s; y = (m01 + m10) / s; z = (m02 + m20) / s;
    }
    else if (m11 > m22)
    {
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        w = (m02 - m20) / s; x = (m01 + m10) / s; y = 0.25 * s; z = (m12 + m21) / s;
    }
    else
    {
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        w = (m10 - m01) / s; x = (m02 + m20) / s; y = (m12 + m21) / s; z = 0.25 * s;
    }
    const double length = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(length > 1e-12)) return std::array<double, 4>{0.0, 0.0, 0.0, 1.0};
    return std::array<double, 4>{x / length, y / length, z / length, w / length};
}

// Smallest rotation angle between two poses, in radians. Used to verify that a generated pose is the
// midpoint of its neighbouring real poses without unwrapping any yaw.
inline double RotationAngle(const HmdMatrix34_t& a, const HmdMatrix34_t& b)
{
    const auto qa = RotationQuaternion(a);
    const auto qb = RotationQuaternion(b);
    double dot = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    dot = std::min(1.0, std::fabs(dot));
    return 2.0 * std::acos(dot);
}

// Midpoint of two poses: slerped rotation, linear translation. This is the exact value a generated
// pose must carry, computed independently of the module's own interpolation.
inline HmdMatrix34_t Midpoint(const HmdMatrix34_t& from, const HmdMatrix34_t& to)
{
    auto qa = RotationQuaternion(from);
    auto qb = RotationQuaternion(to);
    double dot = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    if (dot < 0.0)
    {
        dot = -dot;
        for (double& component : qb) component = -component;
    }
    std::array<double, 4> q{};
    if (dot > 0.9995)
    {
        for (size_t i = 0; i < 4; i++) q[i] = qa[i] + (qb[i] - qa[i]) * 0.5;
        const double length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (length > 1e-12)
            for (double& component : q) component /= length;
    }
    else
    {
        const double theta = std::acos(std::min(1.0, dot));
        const double sinTheta = std::sin(theta);
        const double scaleA = std::sin(0.5 * theta) / sinTheta;
        const double scaleB = std::sin(0.5 * theta) / sinTheta;
        for (size_t i = 0; i < 4; i++) q[i] = qa[i] * scaleA + qb[i] * scaleB;
    }
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    HmdMatrix34_t out{};
    out.m[0][0] = static_cast<float>(1.0 - 2.0 * (y * y + z * z));
    out.m[0][1] = static_cast<float>(2.0 * (x * y - z * w));
    out.m[0][2] = static_cast<float>(2.0 * (x * z + y * w));
    out.m[1][0] = static_cast<float>(2.0 * (x * y + z * w));
    out.m[1][1] = static_cast<float>(1.0 - 2.0 * (x * x + z * z));
    out.m[1][2] = static_cast<float>(2.0 * (y * z - x * w));
    out.m[2][0] = static_cast<float>(2.0 * (x * z - y * w));
    out.m[2][1] = static_cast<float>(2.0 * (y * z + x * w));
    out.m[2][2] = static_cast<float>(1.0 - 2.0 * (x * x + y * y));
    for (int row = 0; row < 3; row++)
        out.m[row][3] = (from.m[row][3] + to.m[row][3]) * 0.5f;
    return out;
}

class FakeVrSystem final : public IVRSystem
{
public:
    static constexpr float DisplayHz = 90.0f;
    static constexpr float VsyncToPhotons = 0.011f;

    DisplayClock clock;
    std::mutex mutex;
    std::vector<HmdMatrix34_t> served;

    size_t ServedCount()
    {
        std::lock_guard<std::mutex> guard(mutex);
        return served.size();
    }

    HmdMatrix34_t ServedPose(size_t index)
    {
        std::lock_guard<std::mutex> guard(mutex);
        return index < served.size() ? served[index] : HmdMatrix34_t{};
    }

    // Analytic head pose: 90 degrees per second of yaw plus a small lateral sway.
    static HmdMatrix34_t PoseAt(double seconds)
    {
        const float yaw = static_cast<float>(seconds * 1.5707963267948966);
        const float sway = 0.05f * std::sin(static_cast<float>(seconds) * 2.0f);
        const float c = std::cos(yaw);
        const float s = std::sin(yaw);
        HmdMatrix34_t pose{};
        pose.m[0][0] = c; pose.m[0][1] = 0.0f; pose.m[0][2] = s; pose.m[0][3] = sway;
        pose.m[1][0] = 0.0f; pose.m[1][1] = 1.0f; pose.m[1][2] = 0.0f; pose.m[1][3] = 0.0f;
        pose.m[2][0] = -s; pose.m[2][1] = 0.0f; pose.m[2][2] = c; pose.m[2][3] = 0.0f;
        return pose;
    }

    HmdMatrix34_t GetEyeToHeadTransform(EVREye eEye) override
    {
        HmdMatrix34_t pose{};
        pose.m[0][0] = 1.0f;
        pose.m[1][1] = 1.0f;
        pose.m[2][2] = 1.0f;
        pose.m[0][3] = eEye == Eye_Left ? -0.032f : 0.032f;
        return pose;
    }

    bool GetTimeSinceLastVsync(float* pfSecondsSinceLastVsync, uint64_t* pulFrameCounter) override
    {
        const double now = clock.Now();
        if (pfSecondsSinceLastVsync != nullptr)
            *pfSecondsSinceLastVsync = static_cast<float>(std::fmod(now, DisplayClock::Period));
        if (pulFrameCounter != nullptr)
            *pulFrameCounter = static_cast<uint64_t>(now / DisplayClock::Period);
        return true;
    }

    void GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin eOrigin,
        float fPredictedSecondsToPhotonsFromNow, TrackedDevicePose_t* pTrackedDevicePoseArray,
        uint32_t unTrackedDevicePoseArrayCount) override
    {
        (void)eOrigin;
        const double at = clock.Now() + fPredictedSecondsToPhotonsFromNow;
        for (uint32_t index = 0; index < unTrackedDevicePoseArrayCount; index++)
        {
            pTrackedDevicePoseArray[index] = TrackedDevicePose_t{};
            if (index != k_unTrackedDeviceIndex_Hmd) continue;
            pTrackedDevicePoseArray[index].mDeviceToAbsoluteTracking = PoseAt(at);
            pTrackedDevicePoseArray[index].bPoseIsValid = true;
            pTrackedDevicePoseArray[index].bDeviceIsConnected = true;
            pTrackedDevicePoseArray[index].eTrackingResult = TrackingResult_Running_OK;
            std::lock_guard<std::mutex> guard(mutex);
            served.push_back(pTrackedDevicePoseArray[index].mDeviceToAbsoluteTracking);
        }
    }

    float GetFloatTrackedDeviceProperty(TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop,
        ETrackedPropertyError* pError) override
    {
        (void)unDeviceIndex;
        if (pError != nullptr) *pError = TrackedProp_Success;
        if (prop == Prop_DisplayFrequency_Float) return DisplayHz;
        if (prop == Prop_SecondsFromVsyncToPhotons_Float) return VsyncToPhotons;
        if (prop == Prop_UserIpdMeters_Float) return 0.064f;
        return 0.0f;
    }

    void GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight ) override {  }
    HmdMatrix44_t GetProjectionMatrix( EVREye eEye, float fNearZ, float fFarZ ) override { return {}; }
    void GetProjectionRaw( EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom ) override {  }
    bool ComputeDistortion( EVREye eEye, float fU, float fV, DistortionCoordinates_t *pDistortionCoordinates ) override { return {}; }
    int32_t GetD3D9AdapterIndex() override { return {}; }
    void GetDXGIOutputInfo( int32_t *pnAdapterIndex ) override {  }
    void GetOutputDevice( uint64_t *pnDevice, ETextureType textureType, VkInstance_T *pInstance) override {  }
    bool IsDisplayOnDesktop() override { return {}; }
    bool SetDisplayVisibility( bool bIsVisibleOnDesktop ) override { return {}; }
    HmdMatrix34_t GetSeatedZeroPoseToStandingAbsoluteTrackingPose() override { return {}; }
    HmdMatrix34_t GetRawZeroPoseToStandingAbsoluteTrackingPose() override { return {}; }
    uint32_t GetSortedTrackedDeviceIndicesOfClass( ETrackedDeviceClass eTrackedDeviceClass, vr::TrackedDeviceIndex_t *punTrackedDeviceIndexArray, uint32_t unTrackedDeviceIndexArrayCount, vr::TrackedDeviceIndex_t unRelativeToTrackedDeviceIndex ) override { return {}; }
    EDeviceActivityLevel GetTrackedDeviceActivityLevel( vr::TrackedDeviceIndex_t unDeviceId ) override { return {}; }
    void ApplyTransform( TrackedDevicePose_t *pOutputPose, const TrackedDevicePose_t *pTrackedDevicePose, const HmdMatrix34_t *pTransform ) override {  }
    vr::TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole( vr::ETrackedControllerRole unDeviceType ) override { return {}; }
    vr::ETrackedControllerRole GetControllerRoleForTrackedDeviceIndex( vr::TrackedDeviceIndex_t unDeviceIndex ) override { return {}; }
    ETrackedDeviceClass GetTrackedDeviceClass( vr::TrackedDeviceIndex_t unDeviceIndex ) override { return {}; }
    bool IsTrackedDeviceConnected( vr::TrackedDeviceIndex_t unDeviceIndex ) override { return {}; }
    bool GetBoolTrackedDeviceProperty( vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError) override { return {}; }
    int32_t GetInt32TrackedDeviceProperty( vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError) override { return {}; }
    uint64_t GetUint64TrackedDeviceProperty( vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError) override { return {}; }
    HmdMatrix34_t GetMatrix34TrackedDeviceProperty( vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError) override { return {}; }
    uint32_t GetArrayTrackedDeviceProperty( vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, PropertyTypeTag_t propType, void *pBuffer, uint32_t unBufferSize, ETrackedPropertyError *pError) override { return {}; }
    uint32_t GetStringTrackedDeviceProperty( vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, char *pchValue, uint32_t unBufferSize, ETrackedPropertyError *pError) override { return {}; }
    const char *GetPropErrorNameFromEnum( ETrackedPropertyError error ) override { return {}; }
    bool PollNextEvent( VREvent_t *pEvent, uint32_t uncbVREvent ) override { return {}; }
    bool PollNextEventWithPose( ETrackingUniverseOrigin eOrigin, VREvent_t *pEvent, uint32_t uncbVREvent, vr::TrackedDevicePose_t *pTrackedDevicePose ) override { return {}; }
    bool PollNextEventWithPoseAndOverlays( vr::ETrackingUniverseOrigin eOrigin, VREvent_t *pEvent, uint32_t uncbVREvent, TrackedDevicePose_t *pTrackedDevicePose, VROverlayHandle_t *pulOverlayHandle ) override { return {}; }
    const char *GetEventTypeNameFromEnum( EVREventType eType ) override { return {}; }
    HiddenAreaMesh_t GetHiddenAreaMesh( EVREye eEye, EHiddenAreaMeshType type ) override { return {}; }
    bool GetControllerState( vr::TrackedDeviceIndex_t unControllerDeviceIndex, vr::VRControllerState_t *pControllerState, uint32_t unControllerStateSize ) override { return {}; }
    bool GetControllerStateWithPose( ETrackingUniverseOrigin eOrigin, vr::TrackedDeviceIndex_t unControllerDeviceIndex, vr::VRControllerState_t *pControllerState, uint32_t unControllerStateSize, TrackedDevicePose_t *pTrackedDevicePose ) override { return {}; }
    void TriggerHapticPulse( vr::TrackedDeviceIndex_t unControllerDeviceIndex, uint32_t unAxisId, unsigned short usDurationMicroSec ) override {  }
    const char *GetButtonIdNameFromEnum( EVRButtonId eButtonId ) override { return {}; }
    const char *GetControllerAxisTypeNameFromEnum( EVRControllerAxisType eAxisType ) override { return {}; }
    bool IsInputAvailable() override { return {}; }
    bool IsSteamVRDrawingControllers() override { return {}; }
    bool ShouldApplicationPause() override { return {}; }
    bool ShouldApplicationReduceRenderingWork() override { return {}; }
    vr::EVRFirmwareError PerformFirmwareUpdate( vr::TrackedDeviceIndex_t unDeviceIndex ) override { return {}; }
    void AcknowledgeQuit_Exiting() override {  }
    uint32_t GetAppContainerFilePaths( char *pchBuffer, uint32_t unBufferSize ) override { return {}; }
    const char *GetRuntimeVersion() override { return {}; }
};

class FakeVrSettings final : public IVRSettings
{
public:
    bool motionSmoothing = false;

    bool GetBool(const char* pchSection, const char* pchSettingsKey, EVRSettingsError* peError) override
    {
        (void)pchSection;
        (void)pchSettingsKey;
        if (peError != nullptr) *peError = VRSettingsError_None;
        return motionSmoothing;
    }

    const char *GetSettingsErrorNameFromEnum( EVRSettingsError eError ) override { return {}; }
    void SetBool( const char *pchSection, const char *pchSettingsKey, bool bValue, EVRSettingsError *peError) override {  }
    void SetInt32( const char *pchSection, const char *pchSettingsKey, int32_t nValue, EVRSettingsError *peError) override {  }
    void SetFloat( const char *pchSection, const char *pchSettingsKey, float flValue, EVRSettingsError *peError) override {  }
    void SetString( const char *pchSection, const char *pchSettingsKey, const char *pchValue, EVRSettingsError *peError) override {  }
    int32_t GetInt32( const char *pchSection, const char *pchSettingsKey, EVRSettingsError *peError) override { return {}; }
    float GetFloat( const char *pchSection, const char *pchSettingsKey, EVRSettingsError *peError) override { return {}; }
    void GetString( const char *pchSection, const char *pchSettingsKey, char *pchValue, uint32_t unValueLen, EVRSettingsError *peError) override {  }
    void RemoveSection( const char *pchSection, EVRSettingsError *peError) override {  }
    void RemoveKeyInSection( const char *pchSection, const char *pchSettingsKey, EVRSettingsError *peError) override {  }
};

class FakeVrCompositor final : public IVRCompositor
{
public:
    struct SubmitRecord
    {
        EVREye eye = Eye_Left;
        ETextureType type = TextureType_Invalid;
        EVRSubmitFlags flags = Submit_Default;
        HmdMatrix34_t pose{};
        ID3D12Resource* resource = nullptr;
        uint32_t arrayIndex = 0;
        // Number of poses the simulated system had served when this submit arrived, so a generated
        // submit is compared against the exact pair of real poses it interpolates.
        size_t servedCount = 0;
    };

    FakeVrSystem* system = nullptr;

    std::mutex mutex;
    std::vector<SubmitRecord> submits;
    std::atomic<uint32_t> submitCalls{0};
    uint32_t waits = 0;
    uint32_t handoffs = 0;
    uint32_t alreadySubmitted = 0;
    DisplayClock clock;

    void SetTrackingSpace(ETrackingUniverseOrigin eOrigin) override { trackingSpace = eOrigin; }
    ETrackingUniverseOrigin GetTrackingSpace() override { return trackingSpace; }

    // A new compositor frame starts: one submit per eye is legal again.
    BIGSCALE_CHECK_NOINLINE EVRCompositorError WaitGetPoses(TrackedDevicePose_t* pRenderPoseArray,
        uint32_t unRenderPoseArrayCount,
        TrackedDevicePose_t* pGamePoseArray, uint32_t unGamePoseArrayCount) override
    {
        (void)pRenderPoseArray;
        (void)unRenderPoseArrayCount;
        (void)pGamePoseArray;
        (void)unGamePoseArrayCount;
        clock.WaitNextBoundary();
        std::lock_guard<std::mutex> guard(mutex);
        waits++;
        submitted_[0] = false;
        submitted_[1] = false;
        return VRCompositorError_None;
    }

    BIGSCALE_CHECK_NOINLINE EVRCompositorError Submit(EVREye eEye, const Texture_t* pTexture,
        const VRTextureBounds_t* pBounds,
        EVRSubmitFlags nSubmitFlags) override
    {
        submitCalls.fetch_add(1, std::memory_order_relaxed);
        return Record(eEye, pTexture, 0, pBounds, nSubmitFlags);
    }

    BIGSCALE_CHECK_NOINLINE EVRCompositorError SubmitWithArrayIndex(EVREye eEye, const Texture_t* pTexture,
        uint32_t unTextureArrayIndex,
        const VRTextureBounds_t* pBounds, EVRSubmitFlags nSubmitFlags) override
    {
        submitCalls.fetch_add(1, std::memory_order_relaxed);
        return Record(eEye, pTexture, unTextureArrayIndex, pBounds, nSubmitFlags);
    }

    BIGSCALE_CHECK_NOINLINE void PostPresentHandoff() override
    {
        std::lock_guard<std::mutex> guard(mutex);
        handoffs++;
    }

    // One same-eye submit per compositor frame: a second one is refused exactly like SteamVR does.
    EVRCompositorError Record(EVREye eye, const Texture_t* texture, uint32_t arrayIndex,
        const VRTextureBounds_t* bounds, EVRSubmitFlags flags);

    EVRCompositorError GetLastPoses( TrackedDevicePose_t* pRenderPoseArray, uint32_t unRenderPoseArrayCount, TrackedDevicePose_t* pGamePoseArray, uint32_t unGamePoseArrayCount ) override { return {}; }
    EVRCompositorError GetLastPoseForTrackedDeviceIndex( TrackedDeviceIndex_t unDeviceIndex, TrackedDevicePose_t *pOutputPose, TrackedDevicePose_t *pOutputGamePose ) override { return {}; }
    EVRCompositorError GetSubmitTexture( Texture_t *pOutTexture, bool *pNeedsFlush, EVRCompositorTextureUsage eUsage, const Texture_t *pTexture, const VRTextureBounds_t *pBounds, EVRSubmitFlags nSubmitFlags ) override { return {}; }
    void ClearLastSubmittedFrame() override {  }
    bool GetFrameTiming( Compositor_FrameTiming *pTiming, uint32_t unFramesAgo) override { return {}; }
    uint32_t GetFrameTimings( Compositor_FrameTiming *pTiming, uint32_t nFrames ) override { return {}; }
    float GetFrameTimeRemaining() override { return {}; }
    void GetCumulativeStats( Compositor_CumulativeStats *pStats, uint32_t nStatsSizeInBytes ) override {  }
    void FadeToColor( float fSeconds, float fRed, float fGreen, float fBlue, float fAlpha, bool bBackground) override {  }
    HmdColor_t GetCurrentFadeColor( bool bBackground) override { return {}; }
    void FadeGrid( float fSeconds, bool bFadeGridIn ) override {  }
    float GetCurrentGridAlpha() override { return {}; }
    EVRCompositorError SetSkyboxOverride( const Texture_t *pTextures, uint32_t unTextureCount ) override { return {}; }
    void ClearSkyboxOverride() override {  }
    void CompositorBringToFront() override {  }
    void CompositorGoToBack() override {  }
    void CompositorQuit() override {  }
    bool IsFullscreen() override { return {}; }
    uint32_t GetCurrentSceneFocusProcess() override { return {}; }
    uint32_t GetLastFrameRenderer() override { return {}; }
    bool CanRenderScene() override { return {}; }
    void ShowMirrorWindow() override {  }
    void HideMirrorWindow() override {  }
    bool IsMirrorWindowVisible() override { return {}; }
    void CompositorDumpImages() override {  }
    bool ShouldAppRenderWithLowResources() override { return {}; }
    void ForceInterleavedReprojectionOn( bool bOverride ) override {  }
    void ForceReconnectProcess() override {  }
    void SuspendRendering( bool bSuspend ) override {  }
    vr::EVRCompositorError GetMirrorTextureD3D11( vr::EVREye eEye, void *pD3D11DeviceOrResource, void **ppD3D11ShaderResourceView ) override { return {}; }
    void ReleaseMirrorTextureD3D11( void *pD3D11ShaderResourceView ) override {  }
    vr::EVRCompositorError GetMirrorTextureGL( vr::EVREye eEye, vr::glUInt_t *pglTextureId, vr::glSharedTextureHandle_t *pglSharedTextureHandle ) override { return {}; }
    bool ReleaseSharedGLTexture( vr::glUInt_t glTextureId, vr::glSharedTextureHandle_t glSharedTextureHandle ) override { return {}; }
    void LockGLSharedTextureForAccess( vr::glSharedTextureHandle_t glSharedTextureHandle ) override {  }
    void UnlockGLSharedTextureForAccess( vr::glSharedTextureHandle_t glSharedTextureHandle ) override {  }
    uint32_t GetVulkanInstanceExtensionsRequired( char *pchValue, uint32_t unBufferSize ) override { return {}; }
    uint32_t GetVulkanDeviceExtensionsRequired( VkPhysicalDevice_T *pPhysicalDevice, char *pchValue, uint32_t unBufferSize ) override { return {}; }
    void SetExplicitTimingMode( EVRCompositorTimingMode eTimingMode ) override {  }
    EVRCompositorError SubmitExplicitTimingData() override { return {}; }
    bool IsMotionSmoothingEnabled() override { return {}; }
    bool IsMotionSmoothingSupported() override { return {}; }
    bool IsCurrentSceneFocusAppLoading() override { return {}; }
    EVRCompositorError SetStageOverride_Async( const char *pchRenderModelPath, const HmdMatrix34_t *pTransform, const Compositor_StageRenderSettings *pRenderSettings, uint32_t nSizeOfRenderSettings) override { return {}; }
    void ClearStageOverride() override {  }
    bool GetCompositorBenchmarkResults( Compositor_BenchmarkResults *pBenchmarkResults, uint32_t nSizeOfBenchmarkResults ) override { return {}; }
    EVRCompositorError GetLastPosePredictionIDs( uint32_t *pRenderPosePredictionID, uint32_t *pGamePosePredictionID ) override { return {}; }
    EVRCompositorError GetPosesForFrame( uint32_t unPosePredictionID, TrackedDevicePose_t* pPoseArray, uint32_t unPoseArrayCount ) override { return {}; }

private:
    ETrackingUniverseOrigin trackingSpace = TrackingUniverseSeated;
    bool submitted_[2] = {false, false};
};

inline EVRCompositorError FakeVrCompositor::Record(EVREye eye, const Texture_t* texture, uint32_t arrayIndex,
    const VRTextureBounds_t* bounds, EVRSubmitFlags flags)
{
    (void)bounds;
    std::lock_guard<std::mutex> guard(mutex);
    const int slot = eye == Eye_Right ? 1 : 0;
    if (submitted_[slot])
    {
        alreadySubmitted++;
        return VRCompositorError_AlreadySubmitted;
    }
    submitted_[slot] = true;
    SubmitRecord record;
    record.eye = eye;
    record.type = texture != nullptr ? texture->eType : TextureType_Invalid;
    record.flags = flags;
    record.arrayIndex = arrayIndex;
    if (texture != nullptr && texture->eType == TextureType_DirectX12 &&
        (flags & Submit_TextureWithPose) != 0)
    {
        const auto* withPose = static_cast<const VRTextureWithPose_t*>(texture);
        record.pose = withPose->mDeviceToAbsoluteTracking;
        const auto* data = static_cast<const D3D12TextureData_t*>(texture->handle);
        record.resource = data != nullptr ? data->m_pResource : nullptr;
    }
    if (system != nullptr) record.servedCount = system->ServedCount();
    submits.push_back(record);
    return VRCompositorError_None;
}
}

#pragma warning(pop)
