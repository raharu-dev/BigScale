using System;
using System.Collections.Generic;
using System.Text;
using BepInEx.Logging;
using HarmonyLib;
using TMPro;
using UnityEngine;
using UnityEngine.UI;

namespace BigScale;

/// <summary>
/// Adds exactly two lines to the game's own hold-TAB details overlay: the observed upscaler
/// conversion and the frame-generation state. The stock FPS counter is decorated in place with the
/// measured presented total.
///
/// Nothing here owns a canvas, polls the TAB key or draws with OnGUI. The text is one TMP child of
/// <c>WorldMenuManager.detailsOverlay</c>, so the overlay's existing visibility, fade and VR canvas
/// parenting apply unchanged. The anchor object is the game's own FPS display; when the hierarchy is
/// not the expected one this logs the real hierarchy once and leaves the stock HUD alone.
/// </summary>
internal sealed class TabStatusHud
{
    private const string ObjectName = "BigScale.HudStatus";
    private const float RefreshSeconds = 0.25f;
    private const float FallbackInset = 16f;

    private static readonly ManualLogSource Log = BepInEx.Logging.Logger.CreateLogSource("BigScale.HUD");

    /// <summary>The live instance, or null before installation/unloading.</summary>
    internal static TabStatusHud? Instance;

    private readonly Action<string> _report;
    private readonly Vector3[] _corners = new Vector3[4];
    private WorldMenuManager? _manager;
    private Transform? _overlay;
    private RectTransform? _canvasRect;
    private FPSDisplay? _fps;
    private TextMeshProUGUI? _text;
    private bool _missingLogged;
    private bool _placementReported;
    private bool _viewFallbackReported;
    private bool _placementLogged;
    private float _nextRefresh;
    private string _writtenRows = "";
    private string _lastStock = "";
    private string _lastDecorated = "";

    internal TabStatusHud(Action<string> report)
    {
        _report = report;
    }

    internal string Status { get; private set; } = "not bound";

    /// <summary>
    /// Binds the current details overlay. Called from the stock manager's own Update, so a scene
    /// replacement is picked up without polling and before the overlay is shown.
    /// </summary>
    private void Bind(WorldMenuManager manager)
    {
        if (manager == null) return;
        _manager = manager;
        Transform? overlay;
        try
        {
            overlay = manager.detailsOverlay;
        }
        catch
        {
            return;
        }
        // The overlay is absent on a title/loading screen. That is normal, never an error.
        if (overlay == null) return;
        if (overlay == _overlay && _text != null && _fps != null) return;
        Rebind(overlay);
    }

    private void Rebind(Transform overlay)
    {
        // A destroyed scene takes the injected text with it; never touch the stock objects.
        DestroyInjected();
        _overlay = overlay;
        _canvasRect = null;
        _fps = null;
        _text = null;
        _writtenRows = "";
        _lastStock = "";
        _lastDecorated = "";
        _nextRefresh = 0f;
        _placementReported = false;
        _viewFallbackReported = false;
        _placementLogged = false;
        try
        {
            // The overlay may be a scaled child of the UI canvas, so placement is measured against
            // the root canvas rectangle and converted back into overlay units on every refresh. The
            // lookup is retried while unresolved: the overlay is often still inactive when binding.
            _canvasRect = ResolveViewRect();
            var fps = overlay.GetComponentInChildren<FPSDisplay>(true);
            var stock = fps != null ? fps.textField : null;
            if (fps == null || stock == null)
            {
                ReportMissingHierarchy(overlay, fps != null);
                return;
            }
            _fps = fps;
            _text = CreateText(overlay, stock);
            _missingLogged = false;
            Status = "bound";
        }
        catch (Exception exception)
        {
            Status = "binding failed";
            _report("could not bind the hold-TAB status rows: " + exception.Message);
        }
    }

    private static TextMeshProUGUI CreateText(Transform overlay, TMP_Text stock)
    {
        var gameObject = new GameObject(ObjectName);
        gameObject.AddComponent<RectTransform>();
        gameObject.transform.SetParent(overlay, false);
        // A stock layout group on the overlay would otherwise reposition the rows; the measured
        // bottom-left placement is this object's contract.
        var layout = gameObject.AddComponent<LayoutElement>();
        if (layout != null) layout.ignoreLayout = true;
        gameObject.SetActive(false);
        var text = gameObject.AddComponent<TextMeshProUGUI>();
        // Copy the stock counter's presentation so the rows read as part of the same overlay.
        text.font = stock.font;
        if (stock.fontSharedMaterial != null) text.fontSharedMaterial = stock.fontSharedMaterial;
        text.fontSize = stock.fontSize;
        text.fontStyle = stock.fontStyle;
        text.color = stock.color;
        text.outlineWidth = stock.outlineWidth;
        text.outlineColor = stock.outlineColor;
        text.characterSpacing = stock.characterSpacing;
        text.wordSpacing = stock.wordSpacing;
        text.lineSpacing = stock.lineSpacing;
        text.paragraphSpacing = stock.paragraphSpacing;
        text.margin = stock.margin;
        text.alignment = TextAlignmentOptions.BottomLeft;
        text.raycastTarget = false;
        text.enableWordWrapping = false;
        text.enableAutoSizing = false;
        text.richText = true;

        // Geometry is applied by UpdatePlacement on the row cadence: the overlay may still be inactive
        // here, so its root canvas rectangle is not measurable yet. The anchors and pivot are ours to
        // own from the start; the first row refresh completes the placement.
        var rect = text.rectTransform;
        rect.anchorMin = Vector2.zero;
        rect.anchorMax = Vector2.zero;
        rect.pivot = Vector2.zero;
        rect.localScale = Vector3.one;
        rect.sizeDelta = Vector2.one;
        rect.anchoredPosition3D = Vector3.zero;
        text.transform.SetAsLastSibling();
        return text;
    }

    /// <summary>
    /// Places the injected rows at the bottom-left of the UI view, reusing the stock counter's own
    /// distance to its nearest edge as the margin. The view is the root canvas rectangle of the
    /// overlay's canvas, so screen pixels, resolution changes, UI scaling and the VR canvas are all
    /// handled by the same measurement. <c>detailsOverlay</c> is a plain <see cref="Transform"/>, so
    /// its own rectangle exists only when it happens to be a RectTransform; the canvas supplies the
    /// view in every other case. Returns false while no view is measurable.
    /// </summary>
    private bool UpdatePlacement()
    {
        var text = _text;
        var overlay = _overlay;
        var stock = _fps != null ? _fps.textField : null;
        var stockRect = stock != null ? stock.rectTransform : null;
        if (text == null || overlay == null || stockRect == null) return false;
        var canvasRect = _canvasRect;
        if (canvasRect == null) canvasRect = _canvasRect = ResolveViewRect();
        var inCanvas = canvasRect != null && IsUsableView(canvasRect.rect);
        var overlayRect = overlay as RectTransform;
        var view = inCanvas ? canvasRect!.rect : overlayRect != null ? overlayRect.rect : default;
        if (!IsUsableView(view)) return false;
        if (inCanvas) _viewFallbackReported = false;
        else ReportViewFallback(overlay, view);

        // Measure in view units: the overlay is only a child of the canvas and may be scaled, and the
        // stock counter may be nested deeper still.
        stockRect.GetWorldCorners(_corners);
        var minX = float.PositiveInfinity;
        var maxX = float.NegativeInfinity;
        var minY = float.PositiveInfinity;
        var maxY = float.NegativeInfinity;
        for (var i = 0; i < _corners.Length; i++)
        {
            var corner = inCanvas
                ? canvasRect!.InverseTransformPoint(_corners[i])
                : overlay.InverseTransformPoint(_corners[i]);
            if (corner.x < minX) minX = corner.x;
            if (corner.x > maxX) maxX = corner.x;
            if (corner.y < minY) minY = corner.y;
            if (corner.y > maxY) maxY = corner.y;
        }
        var inset = NearestInset(minX - view.xMin, view.xMax - maxX, minY - view.yMin, view.yMax - maxY);
        if (!(inset > 0f)) inset = FallbackInset;

        var lowX = view.xMin + inset;
        var highX = view.xMax - inset;
        if (!(highX > lowX)) highX = lowX + 1f;
        var lowY = view.yMin + inset;
        var lineHeight = maxY - minY;
        if (!(lineHeight > 0f)) lineHeight = Mathf.Max(1f, stock != null ? stock.fontSize : 1f);
        var highY = lowY + lineHeight * 2f;

        var bottomLeft = inCanvas
            ? overlay.InverseTransformPoint(canvasRect!.TransformPoint(new Vector3(lowX, lowY, 0f)))
            : new Vector3(lowX, lowY, 0f);
        var bottomRight = inCanvas
            ? overlay.InverseTransformPoint(canvasRect!.TransformPoint(new Vector3(highX, lowY, 0f)))
            : new Vector3(highX, lowY, 0f);
        var topLeft = inCanvas
            ? overlay.InverseTransformPoint(canvasRect!.TransformPoint(new Vector3(lowX, highY, 0f)))
            : new Vector3(lowX, highY, 0f);
        var width = bottomRight.x - bottomLeft.x;
        var height = topLeft.y - bottomLeft.y;
        if (!float.IsFinite(width) || !float.IsFinite(height) ||
            !float.IsFinite(bottomLeft.x) || !float.IsFinite(bottomLeft.y))
            return false;

        // Zero anchors put the pivot at the parent's anchor reference point: a RectTransform parent
        // references its own rectangle's bottom-left corner, while a plain Transform parent references
        // its local origin because its rectangle has no size.
        var anchored = overlayRect != null
            ? new Vector2(bottomLeft.x - overlayRect.rect.xMin, bottomLeft.y - overlayRect.rect.yMin)
            : new Vector2(bottomLeft.x, bottomLeft.y);
        var rect = text.rectTransform;
        rect.anchorMin = Vector2.zero;
        rect.anchorMax = Vector2.zero;
        rect.pivot = Vector2.zero;
        rect.sizeDelta = new Vector2(Mathf.Max(1f, width), Mathf.Max(1f, height));
        rect.anchoredPosition3D = new Vector3(anchored.x, anchored.y, 0f);
        ReportPlacement(overlay, canvasRect, view, inCanvas, inset, rect.sizeDelta, anchored,
            maxX - minX, maxY - minY);
        return true;
    }

    private static bool IsUsableView(Rect view) =>
        view.width > 0f && view.height > 0f && float.IsFinite(view.width) && float.IsFinite(view.height);

    /// <summary>
    /// The rectangle of the UI view this overlay belongs to: the root canvas of the overlay's canvas.
    /// Inactive objects are included because binding happens before the overlay is first shown, and
    /// the search falls back to the highest RectTransform ancestor, which is that same canvas
    /// rectangle for any overlay parented under a canvas.
    /// </summary>
    private RectTransform? ResolveViewRect()
    {
        var overlay = _overlay;
        if (overlay == null) return null;
        var canvas = overlay.GetComponentInParent<Canvas>(true);
        var root = canvas != null ? canvas.rootCanvas : null;
        var rect = root != null ? root.transform as RectTransform : null;
        if (rect != null) return rect;
        var current = overlay;
        while (current != null)
        {
            if (current is RectTransform candidate) rect = candidate;
            current = current.parent;
        }
        return rect;
    }

    /// <summary>Smallest nonnegative distance of the four given edge gaps, or positive infinity.</summary>
    private static float NearestInset(float left, float right, float bottom, float top)
    {
        var nearest = float.PositiveInfinity;
        if (left >= 0f && left < nearest) nearest = left;
        if (right >= 0f && right < nearest) nearest = right;
        if (bottom >= 0f && bottom < nearest) nearest = bottom;
        if (top >= 0f && top < nearest) nearest = top;
        return nearest;
    }

    /// <summary>Records the resolved view and geometry once per placement source.</summary>
    private void ReportPlacement(Transform overlay, RectTransform? canvasRect, Rect view, bool inCanvas,
        float inset, Vector2 size, Vector2 anchored, float stockWidth, float stockHeight)
    {
        if (_placementLogged) return;
        _placementLogged = true;
        Log.LogInfo($"hold-TAB status rows placed: view=" +
                    (inCanvas ? $"{canvasRect!.name} {view.width:F0}x{view.height:F0}" : $"none (overlay rect)") +
                    $", overlay='{overlay.name}' ({(overlay is RectTransform ? "RectTransform" : "Transform")})" +
                    $", stock={stockWidth:F1}x{stockHeight:F1}, inset={inset:F1}" +
                    $", size={size.x:F1}x{size.y:F1}, anchored=({anchored.x:F1}, {anchored.y:F1})");
    }

    /// <summary>Reports once that the rows are placed against the overlay instead of the canvas.</summary>
    private void ReportViewFallback(Transform overlay, Rect view)
    {
        if (_viewFallbackReported) return;
        _viewFallbackReported = true;
        _report("the hold-TAB status rows are placed against the details overlay '" + overlay.name +
                $"' ({view.width:F0}x{view.height:F0}) because no canvas rectangle was measurable; " +
                "margins follow that overlay. The stock HUD is unaffected.");
    }

    private void ReportPlacementUnavailable()
    {
        if (_placementReported) return;
        _placementReported = true;
        var overlay = _overlay;
        var canvas = overlay != null ? overlay.GetComponentInParent<Canvas>(true) : null;
        var view = _canvasRect != null ? _canvasRect.rect : default;
        _report("the hold-TAB status rows have no measurable view yet " +
                $"(overlay={(overlay != null ? overlay.name : "gone")}, " +
                $"rectTransform={overlay is RectTransform}, " +
                $"canvas={(canvas != null ? canvas.name : "none")}, " +
                $"view={view.width:F0}x{view.height:F0}); they keep their last placement. " +
                "The stock HUD is unaffected.");
    }

    private void DestroyInjected()
    {
        if (_text == null) return;
        try
        {
            UnityEngine.Object.Destroy(_text.gameObject);
        }
        catch
        {
            // A scene unload may already have destroyed it; that is the same end state.
        }
        _text = null;
    }

    /// <summary>
    /// Called after the bound game counter refreshed its own text. The measured-total suffix is
    /// repaired on every stock update, because the game rewrites its own label at its own cadence and
    /// would otherwise drop the suffix between row refreshes. Only the rows, their placement and the
    /// status are rebuilt at the bounded cadence.
    /// </summary>
    private void OnFpsUpdated()
    {
        var text = _text;
        var fps = _fps;
        if (text == null || fps == null) return;
        try
        {
            var stockField = fps.textField;
            // A destroyed field defers both jobs; the next stock update retries.
            if (stockField == null) return;
            RepairStockFps(stockField);
            var now = Time.unscaledTime;
            if (now < _nextRefresh) return;
            _nextRefresh = now + RefreshSeconds;
            var coordinator = Plugin.Instance?.Coordinator;
            if (coordinator == null)
            {
                // The renderer is inactive in this environment; leave the stock HUD exactly as is
                // and keep the extra rows empty instead of claiming a state.
                if (_writtenRows.Length > 0)
                {
                    _writtenRows = "";
                    text.text = "";
                }
                return;
            }
            coordinator.BuildHudText(out var upscalerLine, out var frameGenerationLine);
            var rows = upscalerLine + "\n" + frameGenerationLine;
            // Placement is independent of the suffix repair above. An unmeasurable view is reported
            // once and only leaves the rows at their current transform: never regress from
            // visible-but-misplaced to hidden.
            var placed = UpdatePlacement();
            if (!placed) ReportPlacementUnavailable();
            // A success re-arms the one-shot report so a later failure still reports once.
            else _placementReported = false;
            if (!text.gameObject.activeSelf) text.gameObject.SetActive(true);
            if (!string.Equals(rows, _writtenRows, StringComparison.Ordinal) ||
                !string.Equals(text.text, rows, StringComparison.Ordinal))
            {
                _writtenRows = rows;
                text.text = rows;
            }
        }
        catch (Exception exception)
        {
            // A renderer tick failure must not take the overlay down; report once per distinct text.
            var message = "the hold-TAB status rows could not refresh: " + exception.Message;
            if (!string.Equals(Status, message, StringComparison.Ordinal))
            {
                Status = message;
                _report(message);
            }
        }
    }

    /// <summary>
    /// Reapplies the measured total, or restores the undecorated stock value, after the game rewrote
    /// its own label. Runs on every stock update and never depends on the rows or their placement.
    /// </summary>
    private void RepairStockFps(TMP_Text stockField)
    {
        var current = stockField.text ?? "";
        // FPSDisplay may not rewrite its label when the integer did not change. Taking our own
        // decorated output as the base would then append a second total every frame, so the
        // previously observed base is restored first.
        var stock = string.Equals(current, _lastDecorated, StringComparison.Ordinal) &&
                    _lastStock.Length > 0
            ? _lastStock
            : current;
        var coordinator = Plugin.Instance?.Coordinator;
        if (coordinator == null)
        {
            // The renderer is inactive in this environment; give back only our own suffix and stop
            // claiming a base we can no longer decorate.
            if (!string.Equals(stock, current, StringComparison.Ordinal)) stockField.text = stock;
            _lastStock = "";
            _lastDecorated = "";
            return;
        }
        // Effective off and VR ownership restore the localized stock value here, immediately.
        var decorated = coordinator.DecorateStockFps(stock);
        _lastStock = stock;
        _lastDecorated = decorated;
        if (!string.Equals(current, decorated, StringComparison.Ordinal)) stockField.text = decorated;
    }

    private void ReportMissingHierarchy(Transform overlay, bool hasDisplay)
    {
        if (_missingLogged) return;
        _missingLogged = true;
        var names = new StringBuilder();
        for (var i = 0; i < overlay.childCount && i < 12; i++)
        {
            if (i > 0) names.Append(", ");
            names.Append(overlay.GetChild(i).name);
        }
        _report($"the hold-TAB details overlay does not match the expected hierarchy " +
                $"(FPSDisplay={(hasDisplay ? "present" : "missing")}, stock text=" +
                $"{(hasDisplay ? "missing" : "n/a")}); children=[{names}]. The stock HUD is kept unchanged.");
    }

    /// <summary>Destroys only the injected text; the stock overlay and counter are never touched.</summary>
    internal void Dispose()
    {
        DestroyInjected();
        _fps = null;
        _overlay = null;
        _canvasRect = null;
        _manager = null;
        _placementReported = false;
        _viewFallbackReported = false;
        _placementLogged = false;
    }

    /// <summary>Harmony hooks. They report each failure independently, like the settings hooks.</summary>
    internal static class Patches
    {
        internal static void AfterManagerUpdate(WorldMenuManager __instance)
        {
            Instance?.Bind(__instance);
        }

        internal static void AfterFpsUpdate(FPSDisplay __instance)
        {
            var hud = Instance;
            if (hud == null || __instance == null) return;
            var bound = hud._fps;
            if (bound == null || bound.Pointer != __instance.Pointer) return;
            hud.OnFpsUpdated();
        }

        internal static int Install(Harmony harmony, Action<string> report)
        {
            var installed = 0;
            installed += Patch(harmony, typeof(WorldMenuManager), "Update", nameof(AfterManagerUpdate), report);
            installed += Patch(harmony, typeof(FPSDisplay), "Update", nameof(AfterFpsUpdate), report);
            return installed;
        }

        private static int Patch(Harmony harmony, Type type, string method, string patchName,
            Action<string> report)
        {
            try
            {
                var target = AccessTools.Method(type, method);
                if (target == null)
                {
                    report($"HUD hook {type.Name}.{method}: method not found; the stock HUD stays unchanged.");
                    return 0;
                }
                harmony.Patch(target, postfix: new HarmonyMethod(typeof(Patches), patchName));
                return 1;
            }
            catch (Exception exception)
            {
                report($"HUD hook {type.Name}.{method} could not be installed: {exception.Message}");
                return 0;
            }
        }
    }
}
