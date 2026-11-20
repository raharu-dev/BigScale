using System;
using System.Globalization;
using System.Text;

namespace BigScale.Core;

/// <summary>
/// Measures how many frames actually reached the display, and decorates the stock FPS counter with
/// that measured total.
///
/// Frame generation changes the presented rate, not the source rate, so the only truthful total is a
/// delta of the native presentation counters over a real interval. A configuration multiplier is
/// never used as a substitute: it would claim frames the provider did not present. A new identity, a
/// counter rollback or an external suspension reseeds the window without reporting, so a stale rate
/// can never leak across generations, providers, multipliers or a menu suspension.
/// </summary>
public sealed class HudTelemetry
{
    /// <summary>Minimum measurement window; shorter intervals are too noisy to print.</summary>
    public const double MinimumIntervalSeconds = 0.5;

    private bool _hasIdentity;
    private uint _generation;
    private FrameGenerationKind _provider;
    private int _multiplier;
    private bool _seeded;
    private double _seedSeconds;
    private ulong _seedRendered;
    private ulong _seedGenerated;
    private bool _hasRate;
    private int _rate;

    /// <summary>Forgets every sample. Called whenever the HUD must not claim an active rate.</summary>
    public void Reset()
    {
        _hasIdentity = false;
        _seeded = false;
        _hasRate = false;
        _rate = 0;
    }

    /// <summary>
    /// Observes one native snapshot. Returns true when a valid rate is available:
    /// <paramref name="totalFps"/> is then the rounded presented rate, measured over a full window of
    /// the same identity or retained from the last completed window. Intervening sub-window calls of
    /// the same identity keep returning the last measured rate, so the decorated FPS counter does not
    /// drop back to <c>--</c> between measurements. A new identity, a counter rollback or an explicit
    /// reset invalidates the retained rate until another full window completes.
    /// </summary>
    public bool Observe(double nowSeconds, uint generation, FrameGenerationKind provider, int multiplier,
        ulong rendered, ulong generated, out int totalFps)
    {
        totalFps = 0;
        if (!double.IsFinite(nowSeconds)) return false;
        if (!_hasIdentity || generation != _generation || provider != _provider || multiplier != _multiplier)
        {
            _hasIdentity = true;
            _generation = generation;
            _provider = provider;
            _multiplier = multiplier;
            _seeded = false;
            _hasRate = false;
        }
        if (!_seeded)
        {
            _seeded = true;
            _seedSeconds = nowSeconds;
            _seedRendered = rendered;
            _seedGenerated = generated;
            return false;
        }
        // A reset counter (provider restart, generation change, device recovery) is not a rate: it
        // reseeds instead of producing a negative or absurd delta, and it drops the retained rate
        // because nothing measured before the reset describes the current counters.
        if (rendered < _seedRendered || generated < _seedGenerated)
        {
            _seedSeconds = nowSeconds;
            _seedRendered = rendered;
            _seedGenerated = generated;
            _hasRate = false;
            return false;
        }
        var elapsed = nowSeconds - _seedSeconds;
        if (elapsed < MinimumIntervalSeconds)
        {
            // Mid-window: the last completed measurement is still the truthful total for this identity.
            totalFps = _hasRate ? _rate : 0;
            return _hasRate;
        }
        var presented = (rendered - _seedRendered) + (generated - _seedGenerated);
        totalFps = (int)Math.Round(presented / elapsed, MidpointRounding.AwayFromZero);
        _seedSeconds = nowSeconds;
        _seedRendered = rendered;
        _seedGenerated = generated;
        _hasRate = true;
        _rate = totalFps;
        return true;
    }

    /// <summary>
    /// Appends the measured total to the single visible numeric token of the stock counter, keeping
    /// its label and rich-text markup. No token, or more than one ambiguous token, leaves the stock
    /// text untouched; a missing measurement prints <c>--</c>.
    /// </summary>
    public static string DecorateFps(string stockText, int? totalFps)
    {
        if (string.IsNullOrEmpty(stockText)) return stockText;
        var start = -1;
        var length = 0;
        var tokens = 0;
        var inTag = false;
        for (var i = 0; i < stockText.Length; i++)
        {
            var character = stockText[i];
            if (inTag)
            {
                if (character == '>') inTag = false;
                continue;
            }
            if (character == '<')
            {
                inTag = true;
                continue;
            }
            if (!char.IsDigit(character)) continue;
            // A numeric token starts at its first digit and may continue through digits and at most
            // one decimal separator. It must be a maximal run so "1 2" is two ambiguous tokens.
            var end = i;
            var separator = false;
            while (end + 1 < stockText.Length)
            {
                var next = stockText[end + 1];
                if (char.IsDigit(next))
                {
                    end++;
                    continue;
                }
                if (!separator && (next == '.' || next == ','))
                {
                    separator = true;
                    end++;
                    continue;
                }
                break;
            }
            tokens++;
            start = i;
            length = end - i + 1;
            i = end;
            if (tokens > 1) return stockText;
        }
        if (tokens != 1 || start < 0) return stockText;
        var total = totalFps.HasValue && totalFps.Value >= 0
            ? totalFps.Value.ToString(CultureInfo.InvariantCulture)
            : "--";
        var builder = new StringBuilder(stockText.Length + 8);
        builder.Append(stockText, 0, start + length);
        builder.Append('/').Append(total);
        builder.Append(stockText, start + length, stockText.Length - start - length);
        return builder.ToString();
    }
}
