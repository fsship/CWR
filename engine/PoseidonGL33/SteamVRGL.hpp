#pragma once

#include <openvr.h>

#include <string>

// Small OpenVR owner kept private to the GL33 backend. It owns tracking,
// per-eye calibration and the two OpenGL textures submitted to the compositor.
class SteamVRGL
{
  public:
    struct RelativePose
    {
        float rotation[3][3] = {
            {1.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, 1.0f},
        };
        float position[3] = {};
    };

    SteamVRGL() = default;
    ~SteamVRGL();

    SteamVRGL(const SteamVRGL&) = delete;
    SteamVRGL& operator=(const SteamVRGL&) = delete;

    bool Initialize();
    void Shutdown();

    bool IsActive() const { return _active; }
    void UpdateTracking();
    void Recenter();
    bool RequestDiagnosticCapture();
    bool GetRelativePose(RelativePose& pose) const;
    bool GetProjectionTangents(float& horizontal, float& vertical) const;
    bool GetEyeCameraPose(int eye, RelativePose& pose) const;
    bool GetEyeProjectionTangents(int eye, float& left, float& right, float& top, float& bottom) const;
    bool GetRecommendedRenderTargetSize(int& width, int& height) const;

    bool CaptureEye(int eye, unsigned int sourceFramebuffer, unsigned int sourceReadBuffer, int width, int height);
    bool HasStereoFrame() const { return (_capturedEyes & 0x3u) == 0x3u; }
    void SubmitStereoFrame();
    void SubmitFrame(unsigned int sourceFramebuffer, unsigned int sourceReadBuffer, int width, int height);
    void PostPresentHandoff();

  private:
    void UpdateProjectionTangents();
    bool EnsureSubmitTexture(int eye, int width, int height);
    bool CaptureDiagnosticEye(int eye, const std::string& path);
    void LogSubmitErrors(vr::EVRCompositorError left, vr::EVRCompositorError right);

    struct EyeProjection
    {
        float left = 0.0f;
        float right = 0.0f;
        float top = 0.0f;
        float bottom = 0.0f;
    };

    vr::IVRSystem* _system = nullptr;
    vr::IVRCompositor* _compositor = nullptr;
    vr::HmdMatrix34_t _origin = {};
    RelativePose _relativePose;
    RelativePose _eyeCameraPose[2];
    EyeProjection _eyeProjection[2];

    bool _active = false;
    bool _originValid = false;
    bool _poseValid = false;
    bool _trackingWarningLogged = false;

    float _horizontalTan = 0.0f;
    float _verticalTan = 0.0f;
    unsigned int _recommendedWidth = 0;
    unsigned int _recommendedHeight = 0;

    unsigned int _submitTexture[2] = {};
    int _submitWidth[2] = {};
    int _submitHeight[2] = {};
    unsigned int _capturedEyes = 0;
    int _lastSubmitError = 0;
    std::string _diagnosticCaptureBase;
    unsigned int _stereoFrameNumber = 0;
    unsigned int _diagnosticCaptureEarliestFrame = 0;
    unsigned int _diagnosticCaptureSequence = 0;
    bool _diagnosticCaptureRequested = false;
};
