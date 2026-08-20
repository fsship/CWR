#include <PoseidonGL33/SteamVRGL.hpp>

#include <Poseidon/Foundation/Common/GamePaths.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Graphics/Shared/PNGWriter.hpp>

#include <glad/gl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace
{
constexpr int kEngineAxisSign[3] = {1, 1, -1};

SteamVRGL::RelativePose ConvertOpenVRPose(const vr::HmdMatrix34_t& source)
{
    SteamVRGL::RelativePose result;
    for (int row = 0; row < 3; ++row)
    {
        for (int col = 0; col < 3; ++col)
            result.rotation[row][col] =
                static_cast<float>(kEngineAxisSign[row] * kEngineAxisSign[col]) * source.m[row][col];
        result.position[row] = static_cast<float>(kEngineAxisSign[row]) * source.m[row][3];
    }
    return result;
}

}

SteamVRGL::~SteamVRGL()
{
    Shutdown();
}

bool SteamVRGL::Initialize()
{
    if (_active)
        return true;

    vr::EVRInitError error = vr::VRInitError_None;
    _system = vr::VR_Init(&error, vr::VRApplication_Scene);
    if (error != vr::VRInitError_None || !_system)
    {
        LOG_WARN(Graphics, "SteamVR: initialization failed, continuing in desktop mode: {}",
                 vr::VR_GetVRInitErrorAsEnglishDescription(error));
        _system = nullptr;
        return false;
    }

    _compositor = vr::VRCompositor();
    if (!_compositor)
    {
        LOG_WARN(Graphics, "SteamVR: compositor interface is unavailable; continuing in desktop mode");
        vr::VR_Shutdown();
        _system = nullptr;
        return false;
    }

    _compositor->SetTrackingSpace(vr::TrackingUniverseSeated);
    _system->GetRecommendedRenderTargetSize(&_recommendedWidth, &_recommendedHeight);
    _diagnosticCaptureBase = Poseidon::Foundation::AppConfig::Instance().GetVREyeCapturePath();
    if (!_diagnosticCaptureBase.empty())
    {
        _diagnosticCaptureRequested = true;
        _diagnosticCaptureEarliestFrame = 30;
    }
    UpdateProjectionTangents();

    _active = true;
    LOG_INFO(Graphics, "SteamVR: active, recommended per-eye target={}x{}, stereo rendering enabled",
             _recommendedWidth, _recommendedHeight);
    return true;
}

void SteamVRGL::Shutdown()
{
    for (int eye = 0; eye < 2; ++eye)
    {
        if (_submitTexture[eye])
            glDeleteTextures(1, &_submitTexture[eye]);
        _submitTexture[eye] = 0;
        _submitWidth[eye] = 0;
        _submitHeight[eye] = 0;
    }
    _capturedEyes = 0;

    if (_system)
        vr::VR_Shutdown();

    _system = nullptr;
    _compositor = nullptr;
    _active = false;
    _originValid = false;
    _poseValid = false;
}

void SteamVRGL::UpdateProjectionTangents()
{
    if (!_system)
        return;

    float centreHorizontal = 0.0f;
    float centreVertical = 0.0f;
    for (int eyeIndex = 0; eyeIndex < 2; ++eyeIndex)
    {
        const vr::EVREye eye = eyeIndex == 0 ? vr::Eye_Left : vr::Eye_Right;
        float left = 0.0f;
        float right = 0.0f;
        float top = 0.0f;
        float bottom = 0.0f;
        _system->GetProjectionRaw(eye, &left, &right, &top, &bottom);
        _eyeProjection[eyeIndex] = {std::abs(left), std::abs(right), std::abs(top), std::abs(bottom)};
        // OpenVR supplies eye-to-head, which is also the correct local offset
        // for Poseidon's camera-to-world transform. The renderer inverts the
        // completed camera later when it builds the world-to-eye view matrix.
        _eyeCameraPose[eyeIndex] = ConvertOpenVRPose(_system->GetEyeToHeadTransform(eye));

        // The symmetric centre projection remains useful for non-world draws
        // (startup/progress screens) that still submit one image to both eyes.
        centreHorizontal = std::max(centreHorizontal, 0.5f * (std::abs(left) + std::abs(right)));
        centreVertical = std::max(centreVertical, 0.5f * (std::abs(top) + std::abs(bottom)));

        LOG_INFO(Graphics,
                 "SteamVR: {} eye projection L={} R={} T={} B={}, camera eye offset=({}, {}, {})",
                 eyeIndex == 0 ? "left" : "right", _eyeProjection[eyeIndex].left,
                 _eyeProjection[eyeIndex].right, _eyeProjection[eyeIndex].top,
                 _eyeProjection[eyeIndex].bottom, _eyeCameraPose[eyeIndex].position[0],
                 _eyeCameraPose[eyeIndex].position[1], _eyeCameraPose[eyeIndex].position[2]);
    }

    // Reject corrupt/runtime-placeholder values and let the regular game FOV
    // remain in force. Real headset projection tangents comfortably fit here.
    if (centreHorizontal >= 0.1f && centreHorizontal <= 5.0f && centreVertical >= 0.1f && centreVertical <= 5.0f)
    {
        _horizontalTan = centreHorizontal;
        _verticalTan = centreVertical;
        LOG_INFO(Graphics, "SteamVR: centre-eye projection tangents horizontal={} vertical={}",
                 _horizontalTan, _verticalTan);
    }
}

void SteamVRGL::UpdateTracking()
{
    if (!_active || !_compositor)
        return;

    _capturedEyes = 0;

    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount] = {};
    const vr::EVRCompositorError error =
        _compositor->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    if (error != vr::VRCompositorError_None)
    {
        if (!_trackingWarningLogged)
        {
            LOG_WARN(Graphics, "SteamVR: WaitGetPoses failed with compositor error {}", static_cast<int>(error));
            _trackingWarningLogged = true;
        }
        return;
    }

    const vr::TrackedDevicePose_t& hmd = poses[vr::k_unTrackedDeviceIndex_Hmd];
    if (!hmd.bPoseIsValid || !hmd.bDeviceIsConnected)
    {
        if (!_trackingWarningLogged)
        {
            LOG_WARN(Graphics, "SteamVR: HMD pose is not currently valid; retaining the last view pose");
            _trackingWarningLogged = true;
        }
        return;
    }
    _trackingWarningLogged = false;

    const vr::HmdMatrix34_t& current = hmd.mDeviceToAbsoluteTracking;
    if (!_originValid)
    {
        _origin = current;
        _originValid = true;
        _relativePose = RelativePose{};
        _poseValid = true;
        LOG_INFO(Graphics, "SteamVR: seated view recentered from the first valid HMD pose");
        return;
    }

    // OpenVR is right-handed (+Y up, +X right, -Z forward). Poseidon uses
    // +Y up, +X aside, +Z forward. First express the current HMD pose relative
    // to the startup pose (R0^T * Rt), then conjugate it by diag(1,1,-1).
    float relativeVR[3][3] = {};
    for (int row = 0; row < 3; ++row)
    {
        for (int col = 0; col < 3; ++col)
        {
            for (int k = 0; k < 3; ++k)
                relativeVR[row][col] += _origin.m[k][row] * current.m[k][col];
            _relativePose.rotation[row][col] =
                static_cast<float>(kEngineAxisSign[row] * kEngineAxisSign[col]) * relativeVR[row][col];
        }
    }

    float absoluteDelta[3] = {
        current.m[0][3] - _origin.m[0][3],
        current.m[1][3] - _origin.m[1][3],
        current.m[2][3] - _origin.m[2][3],
    };
    float localDelta[3] = {};
    for (int axis = 0; axis < 3; ++axis)
    {
        for (int k = 0; k < 3; ++k)
            localDelta[axis] += _origin.m[k][axis] * absoluteDelta[k];
        _relativePose.position[axis] = static_cast<float>(kEngineAxisSign[axis]) * localDelta[axis];
    }
    _poseValid = true;
}

void SteamVRGL::Recenter()
{
    if (!_active)
        return;

    // The next valid tracked pose becomes the new neutral pose. This resets
    // both orientation and positional room offset without changing the
    // keyboard/mouse-controlled base camera.
    _originValid = false;
    _poseValid = false;
    _relativePose = RelativePose{};
    LOG_INFO(Graphics, "SteamVR: view recenter requested");
}

bool SteamVRGL::RequestDiagnosticCapture()
{
    if (!_active || _diagnosticCaptureRequested)
        return false;

    const std::filesystem::path captureDirectory =
        std::filesystem::path(Poseidon::Foundation::GamePaths::Instance().UserDir()) / "VR Captures";
    std::error_code error;
    std::filesystem::create_directories(captureDirectory, error);
    if (error)
    {
        LOG_WARN(Graphics, "SteamVR: cannot create eye-capture directory '{}': {}", captureDirectory.string(),
                 error.message());
        return false;
    }

    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm localTime = {};
#ifdef _WIN32
    localtime_s(&localTime, &now);
#else
    localtime_r(&now, &localTime);
#endif
    char timestamp[32] = {};
    std::strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &localTime);
    char filename[64] = {};
    std::snprintf(filename, sizeof(filename), "vr-%s-%03u", timestamp, ++_diagnosticCaptureSequence);

    _diagnosticCaptureBase = (captureDirectory / filename).string();
    _diagnosticCaptureEarliestFrame = _stereoFrameNumber + 1;
    _diagnosticCaptureRequested = true;
    LOG_INFO(Graphics, "SteamVR: eye capture requested with prefix '{}'", _diagnosticCaptureBase);
    return true;
}

bool SteamVRGL::GetRelativePose(RelativePose& pose) const
{
    if (!_active || !_poseValid)
        return false;
    pose = _relativePose;
    return true;
}

bool SteamVRGL::GetProjectionTangents(float& horizontal, float& vertical) const
{
    if (!_active || _horizontalTan <= 0.0f || _verticalTan <= 0.0f)
        return false;
    horizontal = _horizontalTan;
    vertical = _verticalTan;
    return true;
}

bool SteamVRGL::GetEyeCameraPose(int eye, RelativePose& pose) const
{
    if (!_active || eye < 0 || eye >= 2)
        return false;
    pose = _eyeCameraPose[eye];
    return true;
}

bool SteamVRGL::GetEyeProjectionTangents(int eye, float& left, float& right, float& top, float& bottom) const
{
    if (!_active || eye < 0 || eye >= 2)
        return false;
    const EyeProjection& projection = _eyeProjection[eye];
    if (projection.left <= 0.0f || projection.right <= 0.0f || projection.top <= 0.0f || projection.bottom <= 0.0f)
        return false;
    left = projection.left;
    right = projection.right;
    top = projection.top;
    bottom = projection.bottom;
    return true;
}

bool SteamVRGL::GetRecommendedRenderTargetSize(int& width, int& height) const
{
    if (!_active || _recommendedWidth == 0 || _recommendedHeight == 0)
        return false;
    width = static_cast<int>(_recommendedWidth);
    height = static_cast<int>(_recommendedHeight);
    return true;
}

bool SteamVRGL::EnsureSubmitTexture(int eye, int width, int height)
{
    if (eye < 0 || eye >= 2 || width <= 0 || height <= 0)
        return false;
    if (_submitTexture[eye] && _submitWidth[eye] == width && _submitHeight[eye] == height)
        return true;

    if (!_submitTexture[eye])
        glGenTextures(1, &_submitTexture[eye]);
    if (!_submitTexture[eye])
        return false;

    GLint previousActiveTexture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE7);
    GLint previousTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    glBindTexture(GL_TEXTURE_2D, _submitTexture[eye]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));

    _submitWidth[eye] = width;
    _submitHeight[eye] = height;
    return true;
}

bool SteamVRGL::CaptureEye(int eye, unsigned int sourceFramebuffer, unsigned int sourceReadBuffer, int width, int height)
{
    if (!_active || !_compositor || !EnsureSubmitTexture(eye, width, height))
        return false;

    GLint previousReadFbo = 0;
    GLint previousReadBuffer = 0;
    GLint previousActiveTexture = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFbo);
    glGetIntegerv(GL_READ_BUFFER, &previousReadBuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFramebuffer);
    glReadBuffer(sourceReadBuffer);
    glActiveTexture(GL_TEXTURE7);
    GLint previousTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    glBindTexture(GL_TEXTURE_2D, _submitTexture[eye]);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFbo));
    glReadBuffer(static_cast<GLenum>(previousReadBuffer));

    _capturedEyes |= 1u << eye;
    return true;
}

bool SteamVRGL::CaptureDiagnosticEye(int eye, const std::string& path)
{
    if (eye < 0 || eye >= 2 || !_submitTexture[eye] || _submitWidth[eye] <= 0 || _submitHeight[eye] <= 0)
        return false;

    const int width = _submitWidth[eye];
    const int height = _submitHeight[eye];
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);

    GLint previousActiveTexture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE7);
    GLint previousTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    glBindTexture(GL_TEXTURE_2D, _submitTexture[eye]);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));

    // Diagnostic images do not need the HMD's full submission resolution.
    // Downsample them 2x with a small box filter while flipping OpenGL's
    // lower-left row order. This keeps headset rendering untouched and makes
    // repeated left/right-eye inspection substantially lighter.
    constexpr int captureScale = 2;
    const int captureWidth = (width + captureScale - 1) / captureScale;
    const int captureHeight = (height + captureScale - 1) / captureScale;
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(captureWidth) * captureHeight * 3);
    for (int y = 0; y < captureHeight; ++y)
    {
        std::uint8_t* destination = rgb.data() + static_cast<std::size_t>(y) * captureWidth * 3;
        for (int x = 0; x < captureWidth; ++x)
        {
            unsigned int sum[3] = {};
            unsigned int samples = 0;
            for (int sampleY = 0; sampleY < captureScale; ++sampleY)
            {
                const int sourceYFromTop = y * captureScale + sampleY;
                if (sourceYFromTop >= height)
                    continue;
                const int sourceY = height - 1 - sourceYFromTop;
                for (int sampleX = 0; sampleX < captureScale; ++sampleX)
                {
                    const int sourceX = x * captureScale + sampleX;
                    if (sourceX >= width)
                        continue;
                    const std::uint8_t* source =
                        rgba.data() + (static_cast<std::size_t>(sourceY) * width + sourceX) * 4;
                    sum[0] += source[0];
                    sum[1] += source[1];
                    sum[2] += source[2];
                    ++samples;
                }
            }
            destination[x * 3 + 0] = static_cast<std::uint8_t>(sum[0] / samples);
            destination[x * 3 + 1] = static_cast<std::uint8_t>(sum[1] / samples);
            destination[x * 3 + 2] = static_cast<std::uint8_t>(sum[2] / samples);
        }
    }

    return Poseidon::PNGWriter::WriteRGB(path.c_str(), captureWidth, captureHeight, rgb.data());
}

void SteamVRGL::LogSubmitErrors(vr::EVRCompositorError left, vr::EVRCompositorError right)
{
    const int submitError = left != vr::VRCompositorError_None ? static_cast<int>(left) : static_cast<int>(right);
    if (submitError != 0 && submitError != _lastSubmitError)
        LOG_WARN(Graphics, "SteamVR: texture submission failed with compositor error {}", submitError);
    _lastSubmitError = submitError;
}

void SteamVRGL::SubmitStereoFrame()
{
    if (!_active || !_compositor || !HasStereoFrame())
        return;

    ++_stereoFrameNumber;
    if (_diagnosticCaptureRequested && !_diagnosticCaptureBase.empty() &&
        _stereoFrameNumber >= _diagnosticCaptureEarliestFrame)
    {
        const std::string leftPath = _diagnosticCaptureBase + "-left.png";
        const std::string rightPath = _diagnosticCaptureBase + "-right.png";
        const bool leftWritten = CaptureDiagnosticEye(0, leftPath);
        const bool rightWritten = CaptureDiagnosticEye(1, rightPath);
        _diagnosticCaptureRequested = false;
        if (leftWritten && rightWritten)
            LOG_INFO(Graphics, "SteamVR: captured diagnostic eye images '{}' and '{}'", leftPath, rightPath);
        else
            LOG_WARN(Graphics, "SteamVR: failed to capture diagnostic eye images using prefix '{}'",
                     _diagnosticCaptureBase);
    }

    glFlush();
    vr::Texture_t leftTexture = {
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(_submitTexture[0])),
        vr::TextureType_OpenGL,
        vr::ColorSpace_Gamma,
    };
    vr::Texture_t rightTexture = {
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(_submitTexture[1])),
        vr::TextureType_OpenGL,
        vr::ColorSpace_Gamma,
    };
    const vr::EVRCompositorError left = _compositor->Submit(vr::Eye_Left, &leftTexture);
    const vr::EVRCompositorError right = _compositor->Submit(vr::Eye_Right, &rightTexture);
    LogSubmitErrors(left, right);
    _capturedEyes = 0;
}

void SteamVRGL::SubmitFrame(unsigned int sourceFramebuffer, unsigned int sourceReadBuffer, int width, int height)
{
    _capturedEyes = 0;
    if (!CaptureEye(0, sourceFramebuffer, sourceReadBuffer, width, height))
        return;

    glFlush();
    vr::Texture_t texture = {
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(_submitTexture[0])),
        vr::TextureType_OpenGL,
        vr::ColorSpace_Gamma,
    };
    const vr::EVRCompositorError left = _compositor->Submit(vr::Eye_Left, &texture);
    const vr::EVRCompositorError right = _compositor->Submit(vr::Eye_Right, &texture);
    LogSubmitErrors(left, right);
    _capturedEyes = 0;
}

void SteamVRGL::PostPresentHandoff()
{
    if (_active && _compositor)
        _compositor->PostPresentHandoff();
}
