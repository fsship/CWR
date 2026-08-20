#include <PoseidonGL33/EngineGL33.hpp>

#if defined(CWR_HAS_OPENVR)
#include <PoseidonGL33/SteamVRGL.hpp>

#include <Poseidon/Foundation/Platform/AppConfig.hpp>

#include <glad/gl.h>

namespace
{
void ApplyLocalPose(Matrix4& transform, const SteamVRGL::RelativePose& pose)
{
    const Matrix3 rotation(
        pose.rotation[0][0], pose.rotation[0][1], pose.rotation[0][2],
        pose.rotation[1][0], pose.rotation[1][1], pose.rotation[1][2],
        pose.rotation[2][0], pose.rotation[2][1], pose.rotation[2][2]);
    const Matrix3 baseOrientation = transform.Orientation();
    const Vector3 localOffset(pose.position[0], pose.position[1], pose.position[2]);

    transform.SetOrientation(baseOrientation * rotation);
    transform.SetPosition(transform.Position() + baseOrientation * localOffset);
}
}

void EngineGL33::InitializeVR()
{
    if (!AppConfig::Instance().UseVR() || !_glContext)
        return;

    _steamVR = std::make_unique<SteamVRGL>();
    if (!_steamVR->Initialize())
    {
        // --vr is deliberately a soft opt-in for this first implementation:
        // a stopped runtime or disconnected headset leaves the desktop game
        // usable instead of turning an otherwise valid launch into a failure.
        _steamVR.reset();
        return;
    }

    // InitGL created the normal window-sized frame target before OpenVR was
    // available. Rebuild it now at the runtime's recommended per-eye size;
    // the default framebuffer remains a small companion mirror.
    DestroySSAATarget();
    ApplyPendingRenderScale();
    BindFrameRenderTarget();
}

void EngineGL33::ShutdownVR()
{
    _vrUIAnchorValid = false;
    _steamVR.reset();
}

bool EngineGL33::IsVREnabled() const
{
    return _steamVR && _steamVR->IsActive();
}

void EngineGL33::UpdateVRTracking()
{
    if (_steamVR)
        _steamVR->UpdateTracking();
}

void EngineGL33::RecenterVRView()
{
    if (_steamVR)
        _steamVR->Recenter();
}

bool EngineGL33::RequestVREyeCapture()
{
    return _steamVR && _steamVR->RequestDiagnosticCapture();
}

void EngineGL33::ApplyVRHeadPose(Matrix4& cameraTransform) const
{
    if (!_steamVR)
        return;

    SteamVRGL::RelativePose pose;
    if (!_steamVR->GetRelativePose(pose))
        return;

    ApplyLocalPose(cameraTransform, pose);
}

bool EngineGL33::GetVRProjectionTangents(float& horizontal, float& vertical) const
{
    return _steamVR && _steamVR->GetProjectionTangents(horizontal, vertical);
}

int EngineGL33::GetVRViewCount() const
{
    return IsVREnabled() ? 2 : 1;
}

void EngineGL33::ApplyVREyeOffset(Matrix4& cameraTransform, int eye) const
{
    if (!_steamVR)
        return;
    SteamVRGL::RelativePose pose;
    if (_steamVR->GetEyeCameraPose(eye, pose))
        ApplyLocalPose(cameraTransform, pose);
}

bool EngineGL33::GetVREyeProjection(int eye, float& left, float& right, float& top, float& bottom) const
{
    return _steamVR && _steamVR->GetEyeProjectionTangents(eye, left, right, top, bottom);
}

void EngineGL33::SetVRUIAnchor(Matrix4Val cameraTransform)
{
    _vrUIAnchor = cameraTransform;
    _vrUIAnchorValid = IsVREnabled();
}

void EngineGL33::CaptureVRView(int eye)
{
    if (!_steamVR || !SSAAActive())
        return;

    ResolveFrameTarget();
    _steamVR->CaptureEye(eye, _ssaaResolveFbo, GL_COLOR_ATTACHMENT0, _ssaaW, _ssaaH);
    // ResolveFrameTarget leaves the resolve FBO bound for drawing. The next
    // eye must start from the multisampled scene target instead.
    BindFrameRenderTarget();
}

bool EngineGL33::GetVRRenderTargetSize(int& width, int& height) const
{
    return _steamVR && _steamVR->GetRecommendedRenderTargetSize(width, height);
}

bool EngineGL33::HasVRStereoFrame() const
{
    return _steamVR && _steamVR->HasStereoFrame();
}

void EngineGL33::SubmitVRFrame(unsigned int sourceFramebuffer, unsigned int sourceReadBuffer, int width, int height)
{
    if (_steamVR)
        _steamVR->SubmitFrame(sourceFramebuffer, sourceReadBuffer, width, height);
}

void EngineGL33::SubmitVRStereoFrame()
{
    if (_steamVR)
        _steamVR->SubmitStereoFrame();
}

void EngineGL33::VRPostPresentHandoff()
{
    if (_steamVR)
        _steamVR->PostPresentHandoff();
}

#else

void EngineGL33::InitializeVR() {}
void EngineGL33::ShutdownVR() { _steamVR.reset(); }
bool EngineGL33::IsVREnabled() const { return false; }
void EngineGL33::UpdateVRTracking() {}
void EngineGL33::RecenterVRView() {}
bool EngineGL33::RequestVREyeCapture() { return false; }
void EngineGL33::ApplyVRHeadPose(Matrix4&) const {}
bool EngineGL33::GetVRProjectionTangents(float&, float&) const { return false; }
int EngineGL33::GetVRViewCount() const { return 1; }
void EngineGL33::ApplyVREyeOffset(Matrix4&, int) const {}
bool EngineGL33::GetVREyeProjection(int, float&, float&, float&, float&) const { return false; }
void EngineGL33::SetVRUIAnchor(Matrix4Val) { _vrUIAnchorValid = false; }
void EngineGL33::CaptureVRView(int) {}
bool EngineGL33::GetVRRenderTargetSize(int&, int&) const { return false; }
bool EngineGL33::HasVRStereoFrame() const { return false; }
void EngineGL33::SubmitVRFrame(unsigned int, unsigned int, int, int) {}
void EngineGL33::SubmitVRStereoFrame() {}
void EngineGL33::VRPostPresentHandoff() {}

#endif
