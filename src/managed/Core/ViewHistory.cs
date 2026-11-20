using System;
using System.Collections.Generic;

namespace BigScale.Core;

/// <summary>One logical eye's immutable retired frame: its own input and output extents.</summary>
public struct RetiredEye
{
    public int Eye;
    public int InputWidth;
    public int InputHeight;
    public int OutputWidth;
    public int OutputHeight;
}

/// <summary>
/// Reusable summary of the newest frame for which every expected eye retired the same provider and
/// preset in the current generation. Allocated once by its owner, never per frame.
/// </summary>
public sealed class RetiredFrameSummary
{
    public const int MaxEyes = 2;

    public readonly RetiredEye[] Eyes = new RetiredEye[MaxEyes];
    public int EyeCount;
    public int FrameIndex = -1;
    public int Generation = -1;
    public UpscalerKind Upscaler;
    public UpscalingPreset Preset;

    public void Clear()
    {
        EyeCount = 0;
        FrameIndex = -1;
        Generation = -1;
        Upscaler = UpscalerKind.Off;
        Preset = UpscalingPreset.Quality;
    }
}

/// <summary>
/// Per-(camera, eye) bookkeeping fed exclusively by native completion records.
/// <para>
/// Two records matter. An <em>executed</em> record means the packet ran on the render thread, so it
/// drives reset bookkeeping. A <em>retired</em> record means the GPU work provably finished, so it is
/// the only thing that can verify activity or release a resource. Delayed GPU retirement must never
/// look like a temporal discontinuity: continuity comes from the executed sequence.
/// </para>
/// </summary>
public sealed class ViewHistory
{
    private sealed class View
    {
        public int LastExecutedFrame = -1;
        public int LastRetiredFrame = -1;
        public int LastRetiredEpoch = -1;
        public int SentEpoch = int.MinValue;
        public long Executions;
        public long Retirements;
        public long Failures;
        public int RetiredInputWidth;
        public int RetiredInputHeight;
        public int RetiredOutputWidth;
        public int RetiredOutputHeight;
        public UpscalerKind RetiredUpscaler = UpscalerKind.Off;
        public UpscalingPreset RetiredPreset = UpscalingPreset.Quality;
    }

    private sealed class Camera
    {
        public int Generation;
        public readonly Dictionary<int, View> Eyes = new();
    }

    private readonly Dictionary<int, Camera> _cameras = new();

    /// <summary>Registers an eye as expected for this camera. Later frames must satisfy it.</summary>
    public void ObserveEye(int cameraId, int eye)
    {
        var camera = CameraFor(cameraId);
        if (!camera.Eyes.ContainsKey(eye)) camera.Eyes[eye] = new View();
    }

    /// <summary>Number of eyes that must report for a frame to count as complete.</summary>
    public int ExpectedEyes(int cameraId) => _cameras.TryGetValue(cameraId, out var camera) ? camera.Eyes.Count : 0;

    /// <summary>History generation shared by every eye of this camera.</summary>
    public int Generation(int cameraId) => _cameras.TryGetValue(cameraId, out var camera) ? camera.Generation : 0;

    /// <summary>Advances the camera generation, invalidating all of its eyes together.</summary>
    public void AdvanceGeneration(int cameraId) => CameraFor(cameraId).Generation++;

    /// <summary>
    /// True the first time an eye is prepared for a new generation, so exactly one explicit history
    /// reset is requested. Frame gaps are handled natively from the executed sequence, never from a
    /// delayed GPU fence.
    /// </summary>
    public bool ConsumeEpochChange(int cameraId, int eye, int epoch)
    {
        var camera = CameraFor(cameraId);
        if (!camera.Eyes.TryGetValue(eye, out var view)) camera.Eyes[eye] = view = new View();
        if (view.SentEpoch == epoch) return false;
        view.SentEpoch = epoch;
        return true;
    }

    /// <summary>Records that a packet ran on the render thread.</summary>
    public void RecordExecution(int cameraId, int eye, int frameIndex, int epoch, bool succeeded)
    {
        var camera = CameraFor(cameraId);
        if (!camera.Eyes.TryGetValue(eye, out var view)) camera.Eyes[eye] = view = new View();
        if (epoch != camera.Generation)
        {
            view.Failures++;
            return;
        }
        if (!succeeded)
        {
            view.Failures++;
            return;
        }
        view.Executions++;
        if (frameIndex > view.LastExecutedFrame) view.LastExecutedFrame = frameIndex;
    }

    /// <summary>Records that a packet's GPU work provably finished.</summary>
    public void RecordRetirement(int cameraId, int eye, int frameIndex, int epoch, bool succeeded,
        UpscalerKind upscaler, UpscalingPreset preset, int inputWidth, int inputHeight, int outputWidth,
        int outputHeight)
    {
        var camera = CameraFor(cameraId);
        if (!camera.Eyes.TryGetValue(eye, out var view)) camera.Eyes[eye] = view = new View();
        if (!succeeded || epoch != camera.Generation)
        {
            view.Failures++;
            if (!succeeded)
            {
                view.LastRetiredFrame = -1;
                view.LastRetiredEpoch = -1;
            }
            return;
        }
        view.Retirements++;
        view.LastRetiredFrame = frameIndex;
        view.LastRetiredEpoch = epoch;
        view.RetiredInputWidth = inputWidth;
        view.RetiredInputHeight = inputHeight;
        view.RetiredOutputWidth = outputWidth;
        view.RetiredOutputHeight = outputHeight;
        view.RetiredUpscaler = upscaler;
        view.RetiredPreset = preset;
    }

    /// <summary>
    /// True only when every expected eye of this camera retired a successful packet for exactly this
    /// frame and this generation. A stale, partial or failed eye never counts.
    /// </summary>
    public bool AllEyesRetired(int cameraId, int frameIndex, int epoch)
    {
        if (!_cameras.TryGetValue(cameraId, out var camera) || camera.Eyes.Count == 0) return false;
        if (camera.Generation != epoch) return false;
        foreach (var view in camera.Eyes.Values)
            if (view.LastRetiredFrame != frameIndex || view.LastRetiredEpoch != epoch) return false;
        return true;
    }

    /// <summary>
    /// Latest frame for which every expected eye retired successfully, or -1 when the eyes disagree,
    /// the generation is stale, or nothing has retired yet.
    /// </summary>
    public int LastFullyRetiredFrame(int cameraId)
    {
        if (!_cameras.TryGetValue(cameraId, out var camera) || camera.Eyes.Count == 0) return -1;
        var frame = -1;
        foreach (var view in camera.Eyes.Values)
        {
            if (view.LastRetiredEpoch != camera.Generation) return -1;
            if (view.LastRetiredFrame < 0) return -1;
            if (frame < 0) frame = view.LastRetiredFrame;
            else if (frame != view.LastRetiredFrame) return -1;
        }
        return frame;
    }

    /// <summary>
    /// Fills <paramref name="summary"/> with the newest fully retired frame. Returns false when the
    /// eyes disagree, the generation is stale, nothing retired, or the eyes did not run the same
    /// provider and preset. The caller compares the provider/preset against the current selection.
    /// </summary>
    public bool TryGetRetiredFrame(int cameraId, RetiredFrameSummary summary)
    {
        summary.Clear();
        if (!_cameras.TryGetValue(cameraId, out var camera) || camera.Eyes.Count == 0) return false;
        if (camera.Eyes.Count > RetiredFrameSummary.MaxEyes) return false;

        var frame = -1;
        UpscalerKind upscaler = UpscalerKind.Off;
        UpscalingPreset preset = UpscalingPreset.Quality;
        var first = true;
        foreach (var view in camera.Eyes.Values)
        {
            if (view.LastRetiredEpoch != camera.Generation) return false;
            if (view.LastRetiredFrame < 0) return false;
            if (first)
            {
                frame = view.LastRetiredFrame;
                upscaler = view.RetiredUpscaler;
                preset = view.RetiredPreset;
                first = false;
            }
            else if (frame != view.LastRetiredFrame || upscaler != view.RetiredUpscaler || preset != view.RetiredPreset)
            {
                return false;
            }
        }

        summary.FrameIndex = frame;
        summary.Generation = camera.Generation;
        summary.Upscaler = upscaler;
        summary.Preset = preset;
        // Deterministic eye order, so a status formatter never depends on dictionary iteration.
        for (var i = 0; i < RetiredFrameSummary.MaxEyes; i++)
        {
            if (!camera.Eyes.TryGetValue(i, out var view)) continue;
            var eye = summary.Eyes[summary.EyeCount++];
            eye.Eye = i;
            eye.InputWidth = view.RetiredInputWidth;
            eye.InputHeight = view.RetiredInputHeight;
            eye.OutputWidth = view.RetiredOutputWidth;
            eye.OutputHeight = view.RetiredOutputHeight;
            summary.Eyes[summary.EyeCount - 1] = eye;
        }
        return summary.EyeCount > 0;
    }

    /// <summary>Latest frame any eye executed, or -1. Used for diagnostics only.</summary>
    public int LastExecutedFrame(int cameraId)
    {
        if (!_cameras.TryGetValue(cameraId, out var camera) || camera.Eyes.Count == 0) return -1;
        var frame = -1;
        foreach (var view in camera.Eyes.Values)
            if (view.LastExecutedFrame > frame) frame = view.LastExecutedFrame;
        return frame;
    }

    /// <summary>Forgets a camera entirely; a replacement object must not inherit its history.</summary>
    public void Forget(int cameraId) => _cameras.Remove(cameraId);

    public void Clear() => _cameras.Clear();

    private Camera CameraFor(int cameraId)
    {
        if (!_cameras.TryGetValue(cameraId, out var camera))
        {
            camera = new Camera();
            _cameras[cameraId] = camera;
        }
        return camera;
    }
}
