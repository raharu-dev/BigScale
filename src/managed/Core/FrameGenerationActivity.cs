using System;

namespace BigScale.Core;

/// <summary>
/// Proves that frame generation actually produced a generated image for the currently requested
/// provider and configuration generation.
///
/// A native <c>Active</c> label alone is not enough: the provider must also have presented at least
/// one generated image above the count that existed when this generation started. Menu suspension
/// reports a non-Active state and deliberately preserves the last proven activity, because the user
/// opening settings must not erase gameplay proof. Faults and provider changes clear it.
/// </summary>
public sealed class FrameGenerationActivity
{
    private uint _generation;
    private FrameGenerationKind _requested = FrameGenerationKind.Off;
    private ulong _baseline;
    private bool _hasBaseline;
    private bool _confirmed;
    private FrameGenerationKind _confirmedProvider = FrameGenerationKind.Off;

    /// <summary>Starts a new proof window for this generation and provider.</summary>
    public void Reset(uint generation, FrameGenerationKind requested, ulong generatedBaseline)
    {
        _generation = generation;
        _requested = requested;
        _baseline = generatedBaseline;
        _hasBaseline = true;
        _confirmed = false;
        _confirmedProvider = FrameGenerationKind.Off;
    }

    /// <summary>
    /// Records one native status observation. Only a matching provider/generation with an Active
    /// state and a generated count above the baseline confirms activity. A non-Active observation
    /// keeps the historical proof instead of erasing it.
    /// </summary>
    public void Observe(uint generation, FrameGenerationKind active, bool activeState, ulong generatedPresents)
    {
        if (!_hasBaseline || generation != _generation || !activeState || active != _requested) return;
        if (generatedPresents <= _baseline) return;
        _confirmed = true;
        _confirmedProvider = active;
    }

    /// <summary>True when this exact generation/provider was confirmed by generated output.</summary>
    public bool Confirmed(uint generation, FrameGenerationKind requested) =>
        _hasBaseline && generation == _generation && requested == _requested && _confirmed;

    /// <summary>The provider that was actually confirmed, or Off before any proof.</summary>
    public FrameGenerationKind ConfirmedProvider => _confirmed ? _confirmedProvider : FrameGenerationKind.Off;
}
