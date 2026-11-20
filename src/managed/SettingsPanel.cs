using System;
using System.Collections.Generic;
using BigScale.Core;
using BepInEx.Logging;
using HarmonyLib;
using Il2CppInterop.Runtime.InteropTypes.Arrays;
using TMPro;
using UnityEngine;
using UnityEngine.UI;

namespace BigScale;

/// <summary>
/// The in-game <c>Upscaling</c> category, built entirely from the game's own uGUI/TMP controls so
/// Big Walk VR's world-space menu and controller pointer handling reach it without extra work.
///
/// Captions and values are bound to the cloned row's own TMP components. The game's serialized
/// <c>SettingsRow.title</c> is frequently null and the visible caption is an unreferenced child named
/// <c>Title</c>, so writing the serialized field alone leaves the template text on screen.
///
/// One panel exists per exact <see cref="SettingsMenu"/> instance. It survives an ordinary
/// disable/re-enable and is disposed only when its menu object is destroyed or the plugin unloads.
/// </summary>
internal sealed class SettingsPanel
{
    private const string CategoryObjectName = "BigScale.Category";
    private const string SettingsObjectName = "BigScale.Settings";
    private const string CategoryLabel = "Upscaling";

    private static readonly ManualLogSource Log = BepInEx.Logging.Logger.CreateLogSource("BigScale.Panel");

    private static readonly Dictionary<int, SettingsPanel> PanelsByRow = new();
    private static readonly Dictionary<int, SettingsPanel> PanelsByMenu = new();
    private static readonly Dictionary<int, SettingsPanel> PanelsByHost = new();

    /// <summary>Menus whose stock <c>Start</c> has run, so building cannot race the game's own setup.</summary>
    private static readonly Dictionary<int, SettingsMenu> StartedMenus = new();

    /// <summary>One build failure per initialized menu; a repeated poll must not retry or re-log it.</summary>
    private static readonly Dictionary<int, string> BuildFailures = new();

    private sealed class RowBinding
    {
        internal SettingsRow Row = null!;
        internal TMP_Text Caption = null!;
        internal TMP_Text Value = null!;
        internal string CaptionText = "";
    }

    private sealed class CategorySnapshot
    {
        internal RectTransform Rect = null!;
        internal Selectable? Selectable;
        internal string Label = "";
        internal Vector2 AnchoredPosition;
        internal Vector2 SizeDelta;
        internal float LocalZ;
        internal Navigation? Navigation;
    }

    /// <summary>
    /// One native layout group on a tab's parent path. Its original enabled state is restored when
    /// the Upscaling tab is released, so the stock column keeps its own layout behaviour after disposal.
    /// </summary>
    private sealed class LayoutGroupSnapshot
    {
        internal LayoutGroup Group = null!;
        internal RectTransform? Rect;
        internal bool Enabled;
    }

    /// <summary>
    /// One tab's geometry as seen from the common column ancestor: the extents that decide the
    /// shared slots plus the local axes that map a common-space slot back into the tab's parent.
    /// </summary>
    private struct TabMeasurement
    {
        internal float Top;
        internal float Bottom;
        internal float CenterY;
        internal float MinX;
        internal float MaxX;
        internal float VerticalAxisY;
        internal float HorizontalSpanY;
        internal float LocalHeight;
        internal float PivotY;
        internal Vector3 UnitUpInCommon;
    }

    private readonly SettingsMenu _menu;
    private readonly SettingsCatagory _catagory;
    private readonly SettingsRow[] _navigation;
    private readonly SettingsRow[] _all;
    private readonly List<RowBinding> _bindings = new();
    private readonly List<CategorySnapshot> _categoryOrder = new();

    private RowBinding _enabledRow = null!;
    private RowBinding _upscalerRow = null!;
    private RowBinding _modeRow = null!;
    private RowBinding _frameGenerationRow = null!;
    private RowBinding _frameMultiplierRow = null!;
    // Whether the FG rows are currently locked by VR. Tracked so a session change restores the stock
    // selectable styling and navigation exactly once.
    private bool _vrLocked;

    private GameObject? _categoryButton;
    private IntPtr _categoryButtonPointer;
    private CategorySnapshot? _categoryButtonSnapshot;
    private CategorySnapshot? _graphicsSnapshot;
    private RectTransform? _categoryParent;
    private readonly List<LayoutGroupSnapshot> _layoutGroups = new();
    private bool _categoryLayoutOwned;
    private bool _categoryLayoutFailureLogged;
    private string _categoryLayoutFailure = "";
    private int _columnId;
    private int _graphicsParentId;
    private bool _distinctTabParents;
    private GameObject? _cloneRoot;
    private bool _localisationStripped;
    private bool _refreshing;
    private bool _disposed;

    private ModSettings Settings => Plugin.Instance!.Settings;
    private RenderCoordinator? Coordinator => Plugin.Instance!.Coordinator;

    private SettingsPanel(SettingsMenu menu, SettingsCatagory catagory, SettingsRow[] navigation, SettingsRow[] all)
    {
        _menu = menu;
        _catagory = catagory;
        _navigation = navigation;
        _all = all;
    }

    /// <summary>The live panel built for this exact menu instance, or null.</summary>
    internal static SettingsPanel? ForMenu(SettingsMenu menu)
    {
        if (menu == null) return null;
        var id = menu.GetInstanceID();
        if (!PanelsByMenu.TryGetValue(id, out var panel) || panel == null)
        {
            PanelsByMenu.Remove(id);
            return null;
        }
        if (panel._disposed || panel._menu != menu)
        {
            PanelsByMenu.Remove(id);
            return null;
        }
        return panel;
    }

    /// <summary>True when the category page is the one currently shown.</summary>
    internal bool IsVisible => !_disposed && _catagory != null && _catagory.gameObject.activeInHierarchy;

    /// <summary>True while any Upscaling page is on screen; used to suspend frame generation.</summary>
    internal static bool AnyVisible
    {
        get
        {
            foreach (var panel in PanelsByMenu.Values)
            {
                if (panel == null || panel._disposed) continue;
                if (panel.IsVisible) return true;
            }
            return false;
        }
    }

    /// <summary>True when this menu was observed through its own <c>Start</c> lifecycle.</summary>
    private static bool HasStarted(SettingsMenu menu)
    {
        if (menu == null) return false;
        var id = menu.GetInstanceID();
        return StartedMenus.TryGetValue(id, out var started) && started == menu;
    }

    /// <summary>Reports why construction failed for this exact menu, without retrying it.</summary>
    internal static bool TryGetBuildFailure(SettingsMenu menu, out string reason)
    {
        reason = "";
        if (menu == null) return false;
        if (!BuildFailures.TryGetValue(menu.GetInstanceID(), out var recorded)) return false;
        reason = recorded;
        return true;
    }

    /// <summary>
    /// Returns the panel for this menu, building it when needed. Building only starts after the game's
    /// own <c>Start</c>, so the stock rows, button list and tab geometry already exist.
    /// </summary>
    internal static SettingsPanel? Ensure(SettingsMenu menu)
    {
        if (menu == null) return null;
        if (!HasStarted(menu)) return null;
        var id = menu.GetInstanceID();
        var existing = ForMenu(menu);
        if (existing != null) return existing;
        if (BuildFailures.ContainsKey(id)) return null;

        SettingsPanel? panel = null;
        string failure;
        try
        {
            panel = Build(menu, out failure);
        }
        catch (Exception exception)
        {
            failure = "build threw " + exception.GetType().Name + ": " + exception.Message;
        }
        if (panel == null)
        {
            BuildFailures[id] = failure;
            Log.LogError("Could not build the Upscaling settings category: " + failure);
            return null;
        }

        // The host component is created only after a successful build, so a failed build leaves no
        // marker behind.
        if (menu.GetComponent<PanelHost>() == null)
        {
            try
            {
                var host = menu.gameObject.AddComponent<PanelHost>();
                PanelsByHost[host.GetInstanceID()] = panel;
            }
            catch (Exception exception)
            {
                Log.LogWarning("Could not attach the Upscaling panel host: " + exception.Message);
            }
        }

        PanelsByMenu[id] = panel;
        return panel;
    }

    private static SettingsPanel? Build(SettingsMenu menu, out string failure)
    {
        var built = BuildCore(menu);
        if (built.Panel != null)
        {
            failure = "";
            return built.Panel;
        }
        failure = built.Failure ?? "the category could not be built";
        return null;
    }

    private static (SettingsPanel? Panel, string? Failure) BuildCore(SettingsMenu menu)
    {
        var source = menu.catagoryGraphics;
        if (source == null) return (null, "the graphics settings category is missing");
        var sourceRows = source.rows;
        if (sourceRows == null || sourceRows.Length < 4)
            return (null, $"the graphics category has {sourceRows?.Length ?? 0} rows; at least 4 are needed");
        if (source.catagoryButton == null) return (null, "the graphics category button is missing");
        if (source.transform.parent == null) return (null, "the graphics category has no parent container");

        // Typed template: an options row has arrows and a visible Variable child. This page has no
        // slider dependency at all, so a stock graphics table without one still builds.
        SettingsRow? optionsTemplate = null;
        foreach (var row in sourceRows)
        {
            if (row == null) continue;
            if (row.slider == null && row.leftButton != null && row.rightButton != null &&
                HasResolvableTmp(row.transform, "Variable"))
                optionsTemplate ??= row;
        }
        if (optionsTemplate == null)
            return (null, "the graphics category has no options row with a Variable label");

        GameObject? staging = null;
        GameObject? cloneRoot = null;
        SettingsPanel? panel = null;

        void Cleanup()
        {
            try
            {
                if (panel != null) panel.Dispose();
            }
            catch (Exception exception)
            {
                Log.LogWarning("Could not clean up a failed Upscaling panel build: " + exception.Message);
            }
            if (cloneRoot != null) UnityEngine.Object.Destroy(cloneRoot);
            if (staging != null) UnityEngine.Object.Destroy(staging);
        }

        // The page, its rows and the tab are all created under an inactive staging root first.
        try
        {
            staging = new GameObject("BigScale.Staging");
            staging.SetActive(false);
            staging.transform.SetParent(source.transform.parent, false);
            cloneRoot = UnityEngine.Object.Instantiate(source.gameObject, staging.transform);
            cloneRoot.name = SettingsObjectName;
            cloneRoot.SetActive(false);

            var catagory = cloneRoot.GetComponent<SettingsCatagory>();
            if (catagory == null)
            {
                Cleanup();
                return (null, "the cloned category lost its SettingsCatagory component");
            }
            var clonedRows = catagory.rows;
            if (clonedRows == null || clonedRows.Length == 0 || clonedRows[0] == null)
            {
                Cleanup();
                return (null, "the cloned category did not reproduce its rows");
            }
            var rowParent = clonedRows[0].transform.parent;
            if (rowParent == null || !rowParent.IsChildOf(cloneRoot.transform))
            {
                Cleanup();
                return (null, "the cloned category's rows are not its own children");
            }
            for (var i = 0; i < clonedRows.Length; i++)
            {
                if (clonedRows[i] == null) continue;
                if (!clonedRows[i].transform.IsChildOf(cloneRoot.transform) ||
                    clonedRows[i].transform.parent != rowParent)
                {
                    Cleanup();
                    return (null, "a cloned category row points outside the cloned page");
                }
                clonedRows[i].gameObject.SetActive(false);
                UnityEngine.Object.Destroy(clonedRows[i].gameObject);
            }

            // The schema is the only source of rows: a control that is not declared there can never
            // appear on the page, even if an older configuration still carries its key.
            var bindings = new Dictionary<string, RowBinding>();
            var ordered = new List<RowBinding>();
            var navigation = new List<SettingsRow>();
            var all = new List<SettingsRow>();
            foreach (var row in SettingsPanelSchema.Rows)
            {
                var binding = CreateBinding(optionsTemplate, staging.transform, row.Key, row.Caption, true,
                    out var bindFailure);
                if (binding == null)
                {
                    Cleanup();
                    return (null, bindFailure);
                }
                bindings[row.Key] = binding;
                ordered.Add(binding);
                all.Add(binding.Row);
                navigation.Add(binding.Row);
            }

            panel = new SettingsPanel(menu, catagory, navigation.ToArray(), all.ToArray())
            {
                _enabledRow = bindings[SettingsPanelSchema.EnabledKey],
                _upscalerRow = bindings[SettingsPanelSchema.UpscalerKey],
                _modeRow = bindings[SettingsPanelSchema.ModeKey],
                _frameGenerationRow = bindings[SettingsPanelSchema.FrameGenerationKey],
                _frameMultiplierRow = bindings[SettingsPanelSchema.FrameMultiplierKey],
                _cloneRoot = cloneRoot,
            };
            panel._bindings.AddRange(ordered);

            foreach (var row in SettingsPanelSchema.Rows)
                panel.Configure(bindings[row.Key]);

            catagory.rows = new Il2CppReferenceArray<SettingsRow>(all.ToArray());

            if (!panel.TryOwnCategoryColumn(source, staging!.transform, out var layoutFailure))
            {
                Cleanup();
                return (null, layoutFailure);
            }

            // Move the finished rows out of staging into the page. The page itself stays inactive until
            // the player opens this category.
            foreach (var binding in panel._bindings)
            {
                binding.Row.transform.SetParent(rowParent, false);
                binding.Row.gameObject.SetActive(true);
            }
            // The staging root was only a construction sandbox. The finished page takes the stock
            // Graphics page's place in the real hierarchy while staying inactive until Show.
            cloneRoot.transform.SetParent(source.transform.parent, false);
            UnityEngine.Object.Destroy(staging);
            staging = null;

            Log.LogInfo($"Upscaling settings category built from {all.Count} bound rows " +
                        $"(menu id={menu.GetInstanceID()}, column id={panel._columnId}, " +
                        $"graphics tab parent id={panel._graphicsParentId}, " +
                        $"distinct tab parents={(panel._distinctTabParents ? "yes" : "no")}).");
            return (panel, null);
        }
        catch (Exception exception)
        {
            Cleanup();
            return (null, "build threw " + exception.GetType().Name + ": " + exception.Message);
        }
    }

    private static bool HasResolvableTmp(Transform row, string name)
    {
        var node = row != null ? row.Find(name) : null;
        if (node == null) return false;
        if (node.GetComponent<TMP_Text>() != null) return true;
        return node.GetComponentsInChildren<TMP_Text>(true).Length == 1;
    }

    private static RowBinding? CreateBinding(SettingsRow template, Transform parent, string name, string caption,
        bool optionsShape, out string failure)
    {
        failure = "";
        var row = CreateRow(template, parent, name);
        if (row == null)
        {
            failure = $"{name}: the row template could not be cloned";
            return null;
        }
        if (!TryBindRow(row, caption, optionsShape ? "Variable" : "VariableDisplay", optionsShape, out var binding,
                out failure))
        {
            UnityEngine.Object.Destroy(row.gameObject);
            return null;
        }
        return binding;
    }

    private static SettingsRow? CreateRow(SettingsRow template, Transform parent, string name)
    {
        if (template == null || parent == null) return null;
        var clone = UnityEngine.Object.Instantiate(template.gameObject, parent);
        clone.name = name;
        clone.SetActive(false);
        return clone.GetComponent<SettingsRow>();
    }

    /// <summary>
    /// Resolves the visible caption and value components on the clone. Both must belong to this row,
    /// and an absent, ambiguous or foreign target aborts the whole build instead of leaving a
    /// half-bound row on screen.
    /// </summary>
    private static bool TryBindRow(SettingsRow row, string caption, string valueNodeName, bool optionsShape,
        out RowBinding binding, out string failure)
    {
        binding = null!;
        failure = "";
        var rowTransform = row.transform;

        TMP_Text? captionText = null;
        var titleNode = rowTransform.Find("Title");
        if (titleNode != null) captionText = titleNode.GetComponent<TMP_Text>();
        if (captionText == null && row.title != null && BelongsTo(row, row.title.transform))
            captionText = row.title.textElement;
        if (captionText == null)
        {
            failure = $"{row.name}: no Title TMP_Text was found on the cloned row";
            return false;
        }
        if (!BelongsTo(row, captionText.transform))
        {
            failure = $"{row.name}: the Title TMP_Text belongs to another object";
            return false;
        }

        TMP_Text? valueText = null;
        var valueNode = rowTransform.Find(valueNodeName);
        if (valueNode != null)
        {
            valueText = valueNode.GetComponent<TMP_Text>();
            if (valueText == null && optionsShape)
            {
                var candidates = valueNode.GetComponentsInChildren<TMP_Text>(true);
                if (candidates.Length > 1)
                {
                    failure = $"{row.name}: {valueNodeName} has {candidates.Length} TMP_Text children";
                    return false;
                }
                if (candidates.Length == 1) valueText = candidates[0];
            }
        }
        if (valueText == null)
        {
            var fallback = optionsShape ? row.arrayLabel : row.sliderLabel;
            if (fallback != null && BelongsTo(row, fallback.transform)) valueText = fallback.textElement;
        }
        if (valueText == null)
        {
            failure = $"{row.name}: no {valueNodeName} TMP_Text was found on the cloned row";
            return false;
        }
        if (!BelongsTo(row, valueText.transform))
        {
            failure = $"{row.name}: the {valueNodeName} TMP_Text belongs to another object";
            return false;
        }
        if (captionText == valueText)
        {
            failure = $"{row.name}: the caption and value resolve to the same TMP_Text";
            return false;
        }

        binding = new RowBinding
        {
            Row = row,
            Caption = captionText,
            Value = valueText,
            CaptionText = caption,
        };
        return true;
    }

    private static bool BelongsTo(SettingsRow row, Transform candidate) =>
        candidate == row.transform || candidate.IsChildOf(row.transform);

    private void Configure(RowBinding binding)
    {
        var row = binding.Row;
        row.saveSelf = false;
        row.settingsType = SettingsType.NotSet;
        row.settingsMenu = _menu;
        row.hanger = null!;
        ClearListeners(row.leftButton);
        ClearListeners(row.rightButton);
        ClearListeners(row.slider);
        PanelsByRow[row.GetInstanceID()] = this;

        var direction = row;
        if (row.leftButton != null)
            row.leftButton.onClick.AddListener(new Action(() => HandleDirection(direction, -1)));
        if (row.rightButton != null)
            row.rightButton.onClick.AddListener(new Action(() => HandleDirection(direction, 1)));
        row.allSelectables = BuildSelectables(row);
    }

    private static Il2CppReferenceArray<Selectable> BuildSelectables(SettingsRow row)
    {
        var selectables = new List<Selectable>();
        if (row.leftButton != null && row.leftButton.gameObject.activeSelf) selectables.Add(row.leftButton);
        if (row.rightButton != null && row.rightButton.gameObject.activeSelf) selectables.Add(row.rightButton);
        if (row.slider != null && row.slider.gameObject.activeSelf) selectables.Add(row.slider);
        return new Il2CppReferenceArray<Selectable>(selectables.ToArray());
    }

    private static void ClearListeners(Button? button)
    {
        if (button == null) return;
        button.onClick = new Button.ButtonClickedEvent();
    }

    private static void ClearListeners(Slider? slider)
    {
        if (slider == null) return;
        slider.onValueChanged = new Slider.SliderEvent();
    }

    /// <summary>Writes literal text directly to the bound TMP component.</summary>
    private static void SetText(TMP_Text? text, string value)
    {
        if (text != null) text.text = value;
    }

    /// <summary>Removes every localisation component from a clone before literal text is written.</summary>
    private static void StripLocalisation(GameObject root)
    {
        if (root == null) return;
        try
        {
            foreach (var label in root.GetComponentsInChildren<LocalizedText>(true))
            {
                if (label == null) continue;
                label.enabled = false;
                UnityEngine.Object.Destroy(label);
            }
        }
        catch (Exception exception)
        {
            Log.LogWarning("Could not remove localisation from a cloned Upscaling control: " + exception.Message);
        }
    }

    /// <summary>Blanks template text that is not a bound caption, value or interactive control.</summary>
    private static void HideUnusedTemplateText(RowBinding binding)
    {
        try
        {
            foreach (var text in binding.Row.GetComponentsInChildren<TMP_Text>(true))
            {
                if (text == null || text == binding.Caption || text == binding.Value) continue;
                if (IsUnder(binding.Row.leftButton, text.transform)) continue;
                if (IsUnder(binding.Row.rightButton, text.transform)) continue;
                if (IsUnder(binding.Row.slider, text.transform)) continue;
                text.text = string.Empty;
            }
        }
        catch (Exception exception)
        {
            Log.LogWarning("Could not hide template text on a Upscaling row: " + exception.Message);
        }
    }

    private static bool IsUnder(Selectable? selectable, Transform candidate) =>
        selectable != null && (candidate == selectable.transform || candidate.IsChildOf(selectable.transform));

    private void RebuildSelectables()
    {
        foreach (var binding in _bindings)
            binding.Row.allSelectables = BuildSelectables(binding.Row);
    }

    /// <summary>
    /// Rebuilds the vertical ring over the rows a navigation may actually land on. While VR owns the
    /// session the multiplier row is visible but excluded and its arrows are non-interactable: VR frame
    /// generation is fixed at 2x, so the remembered desktop multiplier cannot be reached or changed.
    /// </summary>
    private void RebuildNavigation()
    {
        var targets = new List<SettingsRow>(_navigation.Length);
        foreach (var row in _navigation)
        {
            if (_vrLocked && row == _frameMultiplierRow.Row) continue;
            targets.Add(row);
        }
        if (targets.Count == 0) return;
        for (var i = 0; i < targets.Count; i++)
        {
            var previous = targets[(i - 1 + targets.Count) % targets.Count];
            var next = targets[(i + 1) % targets.Count];
            try
            {
                targets[i].SetNavigation(previous, next);
            }
            catch (Exception exception)
            {
                Log.LogWarning("Could not rebuild Upscaling row navigation: " + exception.Message);
            }
        }
        if (!_vrLocked) return;
        // An excluded row is never reachable from another row; point it at itself so a stale
        // selection cannot walk back into the locked control.
        try
        {
            _frameMultiplierRow.Row.SetNavigation(_frameMultiplierRow.Row, _frameMultiplierRow.Row);
        }
        catch (Exception exception)
        {
            Log.LogWarning("Could not lock the Upscaling frame-generation navigation: " + exception.Message);
        }
    }

    /// <summary>
    /// Owns the category column: snapshots the stock tabs, validates the whole placement, disables
    /// the captured layout groups only after that, and adds the Upscaling tab immediately after Graphics.
    /// The stock tabs keep their native parents; the nearest native RectTransform that contains them
    /// all is used only as the measurement and placement coordinate system.
    /// </summary>
    private bool TryOwnCategoryColumn(SettingsCatagory source, Transform staging, out string failure)
    {
        failure = "";
        var sourceButton = source.catagoryButton;
        if (sourceButton == null)
        {
            failure = "the graphics category button is missing";
            return false;
        }
        var graphicsParent = sourceButton.transform.parent;
        if (graphicsParent == null)
        {
            failure = "the graphics category tab has no parent container";
            return false;
        }
        var graphicsParentRect = RectOf(graphicsParent);
        if (graphicsParentRect == null)
        {
            failure = "the graphics category tab has no RectTransform parent (" + Describe(graphicsParent) + ")";
            return false;
        }

        var categories = new (string Label, SettingsCatagory Page)[]
        {
            ("General", _menu.catagoryGeneral),
            ("Interface", _menu.catagoryInterface),
            ("Audio", _menu.catagoryAudio),
            ("Microphone", _menu.catagoryMicrophone),
            ("Graphics", _menu.catagoryGraphics),
            ("Display", _menu.catagoryDisplay),
            ("Controls", _menu.catagoryControls),
            ("Rebind", _menu.catagoryRebind),
        };
        var seenPages = new HashSet<int>();
        var seenButtons = new HashSet<int>();
        var stock = new List<(string Label, Selectable Tab, RectTransform Rect)>();
        foreach (var (label, page) in categories)
        {
            if (page == null)
            {
                failure = $"the {label} category page is missing";
                return false;
            }
            var pageId = page.GetInstanceID();
            if (!seenPages.Add(pageId)) continue;
            var tab = page.catagoryButton;
            if (tab == null)
            {
                failure = $"the {label} category tab is missing (category id={pageId})";
                return false;
            }
            if (!seenButtons.Add(tab.GetInstanceID()))
            {
                failure = $"the {label} category tab is shared with another category ({DescribeTab(tab)})";
                return false;
            }
            var rect = RectOf(tab.transform);
            if (rect == null)
            {
                failure = $"the {label} category tab has no native RectTransform ({DescribeTab(tab)})";
                return false;
            }
            if (tab.GetComponent<Button>() == null)
            {
                failure = $"the {label} category tab has no Button ({DescribeTab(tab)})";
                return false;
            }
            stock.Add((label, tab, rect));
        }

        // Different immediate parents are legal. Walk up from the Graphics tab's parent to the
        // nearest native RectTransform that contains every stock tab; that ancestor is only the
        // shared measurement space, never a new parent for a stock tab.
        RectTransform? common = null;
        var ancestryFailure = "";
        for (var candidate = graphicsParent; candidate != null; candidate = candidate.parent)
        {
            var candidateRect = RectOf(candidate);
            if (candidateRect == null) continue;
            (string Label, Selectable Tab)? offender = null;
            foreach (var entry in stock)
            {
                if (Contains(candidate, entry.Rect.transform)) continue;
                offender = (entry.Label, entry.Tab);
                break;
            }
            if (offender == null)
            {
                common = candidateRect;
                break;
            }
            ancestryFailure = $"the nearest shared ancestor {Describe(candidate)} does not contain the " +
                              $"{offender.Value.Label} category tab ({DescribeTab(offender.Value.Tab)})";
        }
        if (common == null)
        {
            failure = "no native RectTransform contains all eight stock category tabs: " +
                      (ancestryFailure.Length > 0 ? ancestryFailure + "; " : "") +
                      $"graphics tab parent {Describe(graphicsParent)}";
            return false;
        }

        // Keep another mod's category at its existing vertical position; only tabs already in the
        // game's own button list, under the shared ancestor and horizontally in this column qualify.
        var graphicsEntry = stock.Find(entry => SameNative(entry.Tab, sourceButton));
        if (graphicsEntry.Rect == null)
        {
            failure = $"the Graphics category tab could not be identified in the column ({DescribeTab(sourceButton)})";
            return false;
        }
        if (!TryMeasureTab(graphicsEntry.Rect, common, out var graphicsMeasurement, out var measureFailure))
        {
            failure = $"the Graphics category tab cannot be measured: {measureFailure} ({DescribeTab(sourceButton)})";
            return false;
        }

        var extras = new List<(string Label, Selectable Tab, RectTransform Rect)>();
        var known = new HashSet<int>();
        foreach (var entry in stock) known.Add(entry.Tab.GetInstanceID());
        var buttonList = _menu.buttonList;
        if (buttonList != null)
        {
            foreach (var button in buttonList)
            {
                if (button == null) continue;
                if (!known.Add(button.GetInstanceID())) continue;
                var rect = RectOf(button.transform);
                if (rect == null) continue;
                if (rect.gameObject.name == CategoryObjectName) continue;
                if (_categoryButtonPointer != IntPtr.Zero && button.Pointer == _categoryButtonPointer) continue;
                if (!Contains(common, rect.transform)) continue;
                if (!TryMeasureTab(rect, common, out var extraMeasurement, out _)) continue;
                if (!Overlaps(extraMeasurement.MinX, extraMeasurement.MaxX,
                        graphicsMeasurement.MinX, graphicsMeasurement.MaxX)) continue;
                extras.Add((rect.gameObject.name, button, rect));
            }
        }

        var collected = new List<(string Label, Selectable Tab, RectTransform Rect)>(stock);
        collected.AddRange(extras);
        _distinctTabParents = HasDistinctParents(collected);
        _columnId = common.GetInstanceID();
        _graphicsParentId = graphicsParent.GetInstanceID();

        // Settle the stock layout once, before the authoritative measurement and snapshot. Every
        // layout group that is going to be disabled is captured first, so disabling them later
        // cannot leave a group unowned.
        _layoutGroups.Clear();
        _layoutGroups.AddRange(CaptureLayoutGroups(collected, common));
        RebuildCapturedLayouts(_layoutGroups, common);

        var measured = new TabMeasurement[collected.Count];
        for (var i = 0; i < collected.Count; i++)
        {
            if (!TryMeasureTab(collected[i].Rect, common, out measured[i], out var reason))
            {
                failure = $"the {collected[i].Label} category tab cannot be measured: {reason} " +
                          $"({DescribeTab(collected[i].Tab)})";
                return false;
            }
        }

        // Sort by the shared-space top edge; equal edges keep the order they were collected in.
        var order = new List<int>(collected.Count);
        for (var i = 0; i < collected.Count; i++) order.Add(i);
        order.Sort((left, right) =>
        {
            var byTop = measured[right].Top.CompareTo(measured[left].Top);
            return byTop != 0 ? byTop : left.CompareTo(right);
        });

        var graphicsIndex = -1;
        for (var i = 0; i < order.Count; i++)
        {
            if (!SameNative(collected[order[i]].Tab, sourceButton)) continue;
            graphicsIndex = i;
            break;
        }
        if (graphicsIndex < 0)
        {
            failure = "the Graphics category tab could not be identified in the column";
            return false;
        }

        var boundsTop = float.NegativeInfinity;
        var boundsBottom = float.PositiveInfinity;
        foreach (var index in order)
        {
            boundsTop = Mathf.Max(boundsTop, measured[index].Top);
            boundsBottom = Mathf.Min(boundsBottom, measured[index].Bottom);
        }
        if (!IsFinite(boundsTop) || !IsFinite(boundsBottom) || boundsTop <= boundsBottom)
        {
            failure = "the stock category column has no usable vertical bounds";
            return false;
        }

        // Prevalidate every slot, including the proposed Upscaling tab, before any stock geometry changes.
        var finalCount = collected.Count + 1;
        for (var slot = 0; slot < finalCount; slot++)
        {
            var sourceIndex = slot <= graphicsIndex ? order[slot] : order[slot - 1];
            var measurement = measured[sourceIndex];
            var label = slot == graphicsIndex + 1 ? CategoryLabel : collected[sourceIndex].Label;
            if (!CategoryColumnLayout.TryGetSlot(boundsTop, boundsBottom,
                    measurement.Top - measurement.Bottom, finalCount, slot, out var center, out var height))
            {
                failure = $"the stock category column cannot hold {finalCount} tabs ({label} slot {slot}, " +
                          $"{DescribeTab(collected[sourceIndex].Tab)})";
                return false;
            }
            if (!CategoryColumnLayout.TryMapSlot(measurement.CenterY, measurement.LocalHeight,
                    measurement.PivotY, measurement.VerticalAxisY, measurement.HorizontalSpanY, center, height,
                    out _, out _))
            {
                failure = $"the {label} category tab cannot be mapped into slot {slot} " +
                          $"(height={measurement.LocalHeight:F3}, pivotY={measurement.PivotY:F3}, " +
                          $"axisY={measurement.VerticalAxisY:F3}, horizontalSpanY={measurement.HorizontalSpanY:F3}, " +
                          $"{DescribeTab(collected[sourceIndex].Tab)})";
                return false;
            }
        }

        _categoryParent = common;
        _categoryOrder.Clear();
        foreach (var index in order)
            _categoryOrder.Add(Capture(collected[index].Label, collected[index].Rect, collected[index].Tab));
        _graphicsSnapshot = _categoryOrder.Find(snapshot => SameNative(snapshot.Selectable, sourceButton));
        if (_graphicsSnapshot == null)
        {
            failure = "the Graphics category tab could not be identified in the column";
            return false;
        }
        _categoryLayoutOwned = true;
        DisableOwnedLayoutGroups();

        if (!CreateCategoryButton(sourceButton, graphicsParentRect, staging, out failure))
        {
            ReleaseCategoryColumn(false);
            return false;
        }
        if (!ApplyCategoryLayout())
        {
            failure = "the Upscaling category tab could not be placed in the stock column: " + _categoryLayoutFailure;
            ReleaseCategoryColumn(true);
            return false;
        }
        if (_categoryButton != null) _categoryButton.SetActive(true);
        return true;
    }

    private static bool Contains(Transform ancestor, Transform child)
    {
        if (ancestor == null || child == null) return false;
        if (SameNative(ancestor, child)) return true;
        try
        {
            return child.IsChildOf(ancestor);
        }
        catch
        {
            return false;
        }
    }

    private static bool SameNative(UnityEngine.Object? left, UnityEngine.Object? right)
    {
        if (left is null || right is null) return false;
        try
        {
            return left.GetInstanceID() == right.GetInstanceID() && left.Pointer == right.Pointer;
        }
        catch
        {
            return false;
        }
    }

    private static bool Overlaps(float leftMin, float leftMax, float rightMin, float rightMax) =>
        leftMin < rightMax && rightMin < leftMax;

    private static bool HasDistinctParents(List<(string Label, Selectable Tab, RectTransform Rect)> tabs)
    {
        var parents = new HashSet<int>();
        foreach (var tab in tabs)
        {
            var parent = tab.Rect.transform.parent;
            if (parent != null) parents.Add(parent.GetInstanceID());
        }
        return parents.Count > 1;
    }

    /// <summary>
    /// Captures every native layout group between each included tab and the shared ancestor, so the
    /// column only takes over groups it observed and can restore their exact enabled state.
    /// </summary>
    private static List<LayoutGroupSnapshot> CaptureLayoutGroups(
        List<(string Label, Selectable Tab, RectTransform Rect)> tabs, RectTransform common)
    {
        var snapshots = new List<LayoutGroupSnapshot>();
        var seen = new HashSet<int>();
        foreach (var tab in tabs)
        {
            var node = tab.Rect.transform.parent;
            while (node != null)
            {
                LayoutGroup[] groups;
                try
                {
                    groups = node.GetComponents<LayoutGroup>();
                }
                catch
                {
                    groups = Array.Empty<LayoutGroup>();
                }
                foreach (var group in groups)
                {
                    if (group is null) continue;
                    if (!seen.Add(group.GetInstanceID())) continue;
                    snapshots.Add(new LayoutGroupSnapshot
                    {
                        Group = group,
                        Rect = RectOf(node),
                        Enabled = group.enabled,
                    });
                }
                if (SameNative(node, common.transform)) break;
                node = node.parent;
            }
        }
        return snapshots;
    }

    /// <summary>
    /// Forces the stock layout to a settled state before it is measured. Each owned group is rebuilt
    /// once; the shared ancestor is rebuilt once as well when it owns no captured group.
    /// </summary>
    private static void RebuildCapturedLayouts(List<LayoutGroupSnapshot> snapshots, RectTransform common)
    {
        var commonRebuilt = false;
        foreach (var snapshot in snapshots)
        {
            if (!snapshot.Enabled || snapshot.Rect == null) continue;
            try
            {
                LayoutRebuilder.ForceRebuildLayoutImmediate(snapshot.Rect);
            }
            catch
            {
                // A group that cannot rebuild is left to the game's own canvas pass.
            }
            if (SameNative(snapshot.Rect, common)) commonRebuilt = true;
        }
        if (commonRebuilt) return;
        try
        {
            LayoutRebuilder.ForceRebuildLayoutImmediate(common);
        }
        catch
        {
            // The shared ancestor does not have to own a layout group.
        }
    }

    private static bool TryMeasureTab(RectTransform rect, RectTransform common, out TabMeasurement measurement,
        out string reason)
    {
        measurement = default;
        reason = "";
        if (rect == null || common == null)
        {
            reason = "its RectTransform or the shared column is missing";
            return false;
        }
        try
        {
            // All four transformed corners bound the tab in the shared column space, so a rotated or
            // scaled parent is measured by its real extent.
            var local = rect.rect;
            var a = common.InverseTransformPoint(rect.TransformPoint(new Vector3(local.xMin, local.yMin, 0f)));
            var b = common.InverseTransformPoint(rect.TransformPoint(new Vector3(local.xMin, local.yMax, 0f)));
            var c = common.InverseTransformPoint(rect.TransformPoint(new Vector3(local.xMax, local.yMin, 0f)));
            var d = common.InverseTransformPoint(rect.TransformPoint(new Vector3(local.xMax, local.yMax, 0f)));
            var top = Mathf.Max(Mathf.Max(a.y, b.y), Mathf.Max(c.y, d.y));
            var bottom = Mathf.Min(Mathf.Min(a.y, b.y), Mathf.Min(c.y, d.y));
            var minX = Mathf.Min(Mathf.Min(a.x, b.x), Mathf.Min(c.x, d.x));
            var maxX = Mathf.Max(Mathf.Max(a.x, b.x), Mathf.Max(c.x, d.x));
            if (!IsFinite(top) || !IsFinite(bottom) || !IsFinite(minX) || !IsFinite(maxX))
            {
                reason = "its transformed corners are not finite";
                return false;
            }
            if (top <= bottom)
            {
                reason = "it has no measurable height";
                return false;
            }

            var center = common.InverseTransformPoint(rect.TransformPoint(rect.rect.center));
            var unitUp = common.InverseTransformVector(rect.TransformVector(Vector3.up));
            var horizontal = common.InverseTransformVector(rect.TransformVector(Vector3.right));
            var localHeight = local.height;
            var pivotY = rect.pivot.y;
            var horizontalSpan = horizontal.y * local.width;
            if (!IsFinite(center.x) || !IsFinite(center.y) || !IsFinite(center.z) ||
                !IsFinite(unitUp.x) || !IsFinite(unitUp.y) || !IsFinite(unitUp.z) ||
                !IsFinite(horizontalSpan) || !IsFinite(localHeight) || localHeight <= 0f ||
                !IsFinite(pivotY))
            {
                reason = "its local geometry is not finite or has no height";
                return false;
            }
            measurement = new TabMeasurement
            {
                Top = top,
                Bottom = bottom,
                CenterY = center.y,
                MinX = minX,
                MaxX = maxX,
                VerticalAxisY = unitUp.y,
                HorizontalSpanY = Mathf.Abs(horizontalSpan),
                LocalHeight = localHeight,
                PivotY = pivotY,
                UnitUpInCommon = unitUp,
            };
            return true;
        }
        catch
        {
            reason = "its transform could not be read";
            return false;
        }
    }

    private static string Describe(Transform? transform)
    {
        if (transform == null) return "<no transform>";
        try
        {
            var names = new List<string>(8);
            var node = transform;
            var truncated = false;
            for (var depth = 0; depth < 12; depth++)
            {
                if (node == null) break;
                names.Add(string.IsNullOrEmpty(node.name) ? "<unnamed>" : node.name);
                node = node.parent;
                if (node != null && depth == 11) truncated = true;
            }
            names.Reverse();
            var path = string.Join("/", names);
            return $"{(truncated ? "…/" : "")}{path} (id={transform.GetInstanceID()})";
        }
        catch
        {
            return "<unreadable transform>";
        }
    }

    private static string DescribeTab(Selectable? tab)
    {
        if (tab == null) return "<no tab>";
        try
        {
            var button = tab.GetComponent<Button>();
            var id = button != null ? button.GetInstanceID() : tab.GetInstanceID();
            return $"{Describe(tab.transform)} button id={id}";
        }
        catch
        {
            return Describe(tab.transform);
        }
    }

    private bool CreateCategoryButton(Selectable source, RectTransform parent, Transform staging,
        out string failure)
    {
        failure = "";
        GameObject? clone = null;
        try
        {
            clone = UnityEngine.Object.Instantiate(source.gameObject, staging);
            clone.name = CategoryObjectName;
            clone.SetActive(false);
            var selectable = clone.GetComponent<Selectable>();
            var button = clone.GetComponent<Button>();
            if (selectable == null || button == null)
            {
                failure = "the cloned category button has no Selectable/Button";
                UnityEngine.Object.Destroy(clone);
                return false;
            }

            StripLocalisation(clone);
            ClearListeners(button);
            button.onClick.AddListener(new Action(Show));
            var labels = clone.GetComponentsInChildren<TMP_Text>(true);
            if (labels.Length == 0)
            {
                failure = "the cloned category button has no TMP_Text label";
                UnityEngine.Object.Destroy(clone);
                return false;
            }
            foreach (var text in labels)
            {
                SetText(text, CategoryLabel);
                // The stock template may force upper-case/small-caps; the canonical name is mixed
                // case and must render exactly.
                text.fontStyle &= ~(FontStyles.UpperCase | FontStyles.SmallCaps);
            }

            // The tab stays inactive while it moves into the live column, so no stock OnEnable runs
            // before its callbacks and placement are final.
            clone.transform.SetParent(parent, false);

            // The clone must be a real RectTransform before it joins the game's button list, so a
            // failed build cannot leave an unplaceable tab registered with the stock menu.
            var cloneRect = RectOf(clone.transform);
            if (cloneRect == null)
            {
                failure = "the cloned category tab has no RectTransform";
                UnityEngine.Object.Destroy(clone);
                return false;
            }

            var list = _menu.buttonList;
            if (list == null)
            {
                failure = "the game's category button list is not available";
                UnityEngine.Object.Destroy(clone);
                return false;
            }
            var sourcePointer = source.Pointer;
            var listIndex = -1;
            for (var i = 0; i < list.Count; i++)
            {
                var candidate = list[i];
                if (candidate == null || candidate.Pointer != sourcePointer) continue;
                listIndex = i;
                break;
            }
            if (listIndex < 0)
            {
                failure = "the Graphics category tab is not in the game's button list (" +
                          DescribeTab(source) + ")";
                UnityEngine.Object.Destroy(clone);
                return false;
            }
            // Cache the clone's Button component pointer before it joins the list; removal uses the
            // same identity even after the Unity object is destroyed.
            _categoryButtonPointer = button.Pointer;
            list.Insert(Mathf.Clamp(listIndex + 1, 0, list.Count), button);
            clone.transform.SetSiblingIndex(Mathf.Min(source.transform.GetSiblingIndex() + 1,
                parent.childCount - 1));

            _categoryButton = clone;
            _categoryButtonSnapshot = Capture(CategoryLabel, cloneRect, selectable);
            _catagory.catagoryButton = selectable;
            return true;
        }
        catch (Exception exception)
        {
            RemoveOwnedCategoryButtonEntry();
            if (clone != null) UnityEngine.Object.Destroy(clone);
            _categoryButton = null;
            _categoryButtonSnapshot = null;
            failure = "the category tab could not be created: " + exception.GetType().Name + ": " +
                      exception.Message;
            return false;
        }
    }

    private static CategorySnapshot Capture(string label, RectTransform rect, Selectable? selectable) =>
        new()
        {
            Rect = rect,
            Selectable = selectable,
            Label = label,
            AnchoredPosition = rect.anchoredPosition,
            SizeDelta = rect.sizeDelta,
            LocalZ = rect.localPosition.z,
            Navigation = selectable != null ? selectable.navigation : null,
        };

    private static void Restore(CategorySnapshot? snapshot)
    {
        if (snapshot?.Rect == null) return;
        snapshot.Rect.sizeDelta = snapshot.SizeDelta;
        snapshot.Rect.anchoredPosition = snapshot.AnchoredPosition;
        var local = snapshot.Rect.localPosition;
        if (!Mathf.Approximately(local.z, snapshot.LocalZ))
        {
            local.z = snapshot.LocalZ;
            snapshot.Rect.localPosition = local;
        }
        if (snapshot.Selectable != null && snapshot.Navigation != null)
            snapshot.Selectable.navigation = snapshot.Navigation;
    }

    /// <summary>
    /// Restores the stock baseline, then places every surviving tab and the Upscaling tab inside the
    /// shared column. Every mapping is measured and validated before the first RectTransform changes;
    /// a failure restores the checkpoint captured from the previously usable layout.
    /// </summary>
    internal bool ApplyCategoryLayout()
    {
        _categoryLayoutFailure = "";
        if (_disposed || !_categoryLayoutOwned || _categoryParent == null) return false;
        if (_categoryButtonSnapshot == null || _categoryButtonSnapshot.Rect == null)
        {
            FailLayout(new List<CategorySnapshot>(), "the Upscaling category tab is missing");
            return false;
        }

        var checkpoint = new List<CategorySnapshot>(_categoryOrder.Count + 1);
        try
        {
            foreach (var snapshot in _categoryOrder)
                if (snapshot.Rect != null) checkpoint.Add(Capture(snapshot.Label, snapshot.Rect, snapshot.Selectable));
            checkpoint.Add(Capture(CategoryLabel, _categoryButtonSnapshot.Rect, _categoryButtonSnapshot.Selectable));

            foreach (var snapshot in _categoryOrder) Restore(snapshot);
            Restore(_categoryButtonSnapshot);
            DisableOwnedLayoutGroups();

            var survivors = new List<CategorySnapshot>(_categoryOrder.Count);
            foreach (var snapshot in _categoryOrder)
                if (snapshot.Rect != null) survivors.Add(snapshot);
            if (survivors.Count == 0)
                return FailLayout(checkpoint, "no stock category tab survives");

            var graphicsIndex = survivors.IndexOf(_graphicsSnapshot!);
            if (graphicsIndex < 0)
                return FailLayout(checkpoint, "the Graphics category tab disappeared");

            var ordered = new List<CategorySnapshot>(survivors.Count + 1);
            for (var i = 0; i < survivors.Count; i++)
            {
                ordered.Add(survivors[i]);
                if (i == graphicsIndex) ordered.Add(_categoryButtonSnapshot);
            }

            var count = ordered.Count;
            var measured = new TabMeasurement[count];
            var boundsTop = float.NegativeInfinity;
            var boundsBottom = float.PositiveInfinity;
            for (var i = 0; i < count; i++)
            {
                var rect = ordered[i].Rect;
                if (rect == null)
                    return FailLayout(checkpoint, $"the {ordered[i].Label} category tab has no RectTransform");
                if (!TryMeasureTab(rect, _categoryParent, out measured[i], out var reason))
                    return FailLayout(checkpoint, $"the {ordered[i].Label} category tab cannot be measured: " +
                                                 $"{reason} ({Describe(rect.transform)})");
                boundsTop = Mathf.Max(boundsTop, measured[i].Top);
                boundsBottom = Mathf.Min(boundsBottom, measured[i].Bottom);
            }
            if (!IsFinite(boundsTop) || !IsFinite(boundsBottom) || boundsTop <= boundsBottom)
                return FailLayout(checkpoint, "the stock category column has no usable vertical bounds");

            // Precompute every placement, so a rejected slot cannot leave a partially laid out column.
            var mappedHeights = new float[count];
            var translations = new Vector3[count];
            for (var i = 0; i < count; i++)
            {
                var label = ordered[i].Label;
                var measurement = measured[i];
                if (!CategoryColumnLayout.TryGetSlot(boundsTop, boundsBottom, measurement.Top - measurement.Bottom,
                        count, i, out var center, out var slotHeight))
                    return FailLayout(checkpoint, $"the {label} category tab has no slot {i} in the stock column");

                if (!CategoryColumnLayout.TryMapSlot(measurement.CenterY, measurement.LocalHeight,
                        measurement.PivotY, measurement.VerticalAxisY, measurement.HorizontalSpanY, center,
                        slotHeight, out mappedHeights[i], out var columnOffsetY))
                    return FailLayout(checkpoint, $"the {label} category tab cannot be mapped into slot {i} " +
                                                 $"(height={measurement.LocalHeight:F3}, " +
                                                 $"pivotY={measurement.PivotY:F3}, " +
                                                 $"axisY={measurement.VerticalAxisY:F3}, " +
                                                 $"horizontalSpanY={measurement.HorizontalSpanY:F3}, " +
                                                 $"{Describe(ordered[i].Rect.transform)})");

                var pivotShift = measurement.UnitUpInCommon *
                                 ((mappedHeights[i] - measurement.LocalHeight) * (0.5f - measurement.PivotY));
                translations[i] = new Vector3(-pivotShift.x, columnOffsetY, -pivotShift.z);
                if (!IsFinite(mappedHeights[i]) || mappedHeights[i] <= 0f ||
                    !IsFinite(translations[i].x) || !IsFinite(translations[i].y) || !IsFinite(translations[i].z))
                    return FailLayout(checkpoint, $"the {label} category tab has no finite placement");
            }

            for (var i = 0; i < count; i++)
            {
                var rect = ordered[i].Rect;
                rect.SetSizeWithCurrentAnchors(RectTransform.Axis.Vertical, mappedHeights[i]);
                rect.position += _categoryParent.TransformVector(translations[i]);
            }

            for (var i = 0; i < count; i++)
            {
                var selectable = ordered[i].Selectable;
                if (selectable == null) continue;
                var navigation = selectable.navigation;
                if (navigation == null) navigation = new Navigation();
                navigation.mode = Navigation.Mode.Explicit;
                navigation.selectOnUp = i > 0 ? ordered[i - 1].Selectable : null;
                navigation.selectOnDown = i < count - 1 ? ordered[i + 1].Selectable : null;
                selectable.navigation = navigation;
            }
            _categoryLayoutFailureLogged = false;
            return true;
        }
        catch (Exception exception)
        {
            return FailLayout(checkpoint, "layout threw " + exception.GetType().Name + ": " + exception.Message);
        }
    }

    private bool FailLayout(List<CategorySnapshot> checkpoint, string reason)
    {
        _categoryLayoutFailure = reason;
        try
        {
            foreach (var snapshot in checkpoint) Restore(snapshot);
        }
        catch (Exception exception)
        {
            Log.LogWarning("Could not restore the previous Upscaling category layout: " + exception.Message);
        }
        if (!_categoryLayoutFailureLogged)
        {
            _categoryLayoutFailureLogged = true;
            Log.LogWarning("Could not place the Upscaling category tab: " + reason +
                           "; the previous category layout is kept.");
        }
        return false;
    }

    /// <summary>
    /// Releases the column back to its original baseline. Safe to call twice: the second call has no
    /// snapshots left, so it cannot re-lay-out the stock controls.
    /// </summary>
    private void ReleaseCategoryColumn(bool destroyButton)
    {
        if (_categoryParent != null)
        {
            foreach (var snapshot in _categoryOrder) Restore(snapshot);
            Restore(_categoryButtonSnapshot);
            RestoreOwnedLayoutGroups();
        }

        _categoryOrder.Clear();
        _layoutGroups.Clear();
        _categoryParent = null;
        _graphicsSnapshot = null;
        _categoryButtonSnapshot = null;
        _categoryLayoutOwned = false;
        _categoryLayoutFailureLogged = false;
        _categoryLayoutFailure = "";
        _columnId = 0;
        _graphicsParentId = 0;
        _distinctTabParents = false;

        if (!destroyButton) return;

        // Remove the owned entry before destroying the object; the cached Button component pointer is
        // the only identity that still matches after the Unity object becomes destroyed.
        RemoveOwnedCategoryButtonEntry();
        if (_categoryButton != null)
        {
            try
            {
                UnityEngine.Object.Destroy(_categoryButton);
            }
            catch (Exception exception)
            {
                Log.LogWarning("Could not destroy the Upscaling category tab: " + exception.Message);
            }
        }
        _categoryButton = null;
    }

    private void RemoveOwnedCategoryButtonEntry()
    {
        var pointer = _categoryButtonPointer;
        _categoryButtonPointer = IntPtr.Zero;
        if (pointer == IntPtr.Zero) return;
        try
        {
            var list = _menu.buttonList;
            if (list == null) return;
            for (var i = list.Count - 1; i >= 0; i--)
            {
                var candidate = list[i];
                if (candidate is null) continue;
                IntPtr candidatePointer;
                try
                {
                    candidatePointer = candidate.Pointer;
                }
                catch
                {
                    continue;
                }
                if (candidatePointer == pointer) list.RemoveAt(i);
            }
        }
        catch (Exception exception)
        {
            Log.LogWarning("Could not remove the Upscaling category tab from the button list: " + exception.Message);
        }
    }

    private void DisableOwnedLayoutGroups()
    {
        foreach (var snapshot in _layoutGroups)
        {
            if (snapshot.Group == null) continue;
            try
            {
                snapshot.Group.enabled = false;
            }
            catch
            {
                // The group is already being torn down.
            }
        }
    }

    private void RestoreOwnedLayoutGroups()
    {
        var anyEnabled = false;
        foreach (var snapshot in _layoutGroups)
        {
            if (snapshot.Group == null) continue;
            try
            {
                snapshot.Group.enabled = snapshot.Enabled;
                if (snapshot.Enabled) anyEnabled = true;
            }
            catch
            {
                // The group is already being torn down.
            }
        }
        if (!anyEnabled || _categoryParent == null) return;
        try
        {
            LayoutRebuilder.ForceRebuildLayoutImmediate(_categoryParent);
        }
        catch
        {
            // The restored values remain even when the common ancestor cannot rebuild.
        }
    }

    /// <summary>
    /// Resolves a <see cref="RectTransform"/> through its native component. IL2CPP returns declared
    /// <see cref="Transform"/> wrappers from <c>transform</c>/<c>parent</c>; a CLR downcast can fail
    /// even when the native object is a RectTransform, so ask the native component system instead.
    /// </summary>
    private static RectTransform? RectOf(Transform? transform)
    {
        if (transform == null) return null;
        try
        {
            var gameObject = transform.gameObject;
            if (gameObject == null) return null;
            return gameObject.GetComponent<RectTransform>();
        }
        catch
        {
            return null;
        }
    }

    private static bool IsFinite(float value) => !float.IsNaN(value) && !float.IsInfinity(value);

    /// <summary>Shows this category through the game's own category swap.</summary>
    internal void Show()
    {
        if (_disposed) return;
        ApplyCategoryLayout();
        try
        {
            _menu.SwapToCatagory(_catagory);
        }
        catch (Exception exception)
        {
            Log.LogWarning("SwapToCatagory failed, falling back to NavigateToCatagory: " + exception.Message);
            try
            {
                _menu.NavigateToCatagory(_catagory);
            }
            catch (Exception inner)
            {
                Log.LogError("Could not open the Upscaling category: " + inner.Message);
                return;
            }
        }
        try
        {
            _catagory.gameObject.SetActive(true);
        }
        catch (Exception exception)
        {
            Log.LogWarning("Could not activate the Upscaling category page: " + exception.Message);
            return;
        }

        // The stock lifecycle may have laid the column out again while activating the page.
        ApplyCategoryLayout();
        // SettingsCatagory.Start runs on this first activation; reassert the page's own rows and tab
        // so a stock field write cannot replace them.
        _catagory.rows = new Il2CppReferenceArray<SettingsRow>(_all);
        if (_categoryButton != null)
        {
            var selectable = _categoryButton.GetComponent<Selectable>();
            if (selectable != null) _catagory.catagoryButton = selectable;
            EnsureCategoryButtonListed();
        }

        // The page's own OnEnable has now run. Remove localisation so it cannot restore template text,
        // then write the bound literals synchronously before the first rendered frame.
        if (!_localisationStripped)
        {
            _localisationStripped = true;
            foreach (var binding in _bindings)
            {
                StripLocalisation(binding.Row.gameObject);
                HideUnusedTemplateText(binding);
            }
        }
        RebuildSelectables();
        RebuildNavigation();
        FocusFirstRow();
        // The row must say "suspended for settings" immediately, not after the bounded refresh.
        Coordinator?.RefreshFrameGenerationStatus(true);
        Refresh();
    }

    /// <summary>Hides the page without disposing anything, for an ordinary settings close.</summary>
    internal void Hide()
    {
        if (_disposed) return;
        try
        {
            if (_catagory != null) _catagory.gameObject.SetActive(false);
        }
        catch (Exception exception)
        {
            Log.LogWarning("Could not hide the Upscaling category: " + exception.Message);
        }
        Coordinator?.RefreshFrameGenerationStatus(false);
        Refresh();
    }

    /// <summary>Returns to a previously active category, or leaves this page hidden.</summary>
    internal void RestoreCategory(SettingsCatagory? previous)
    {
        if (_disposed) return;
        if (previous != null && previous != _catagory && previous.gameObject != null)
        {
            try
            {
                _menu.SwapToCatagory(previous);
                return;
            }
            catch (Exception exception)
            {
                Log.LogWarning("Could not restore the previous category: " + exception.Message);
            }
        }
        Hide();
    }

    private void FocusFirstRow()
    {
        try
        {
            var events = UnityEngine.EventSystems.EventSystem.current;
            if (events == null) return;
            foreach (var row in _navigation)
            {
                if (row == null) continue;
                var first = row.firstSelectable;
                if (first == null)
                {
                    var selectables = row.allSelectables;
                    if (selectables != null && selectables.Length > 0) first = selectables[0];
                }
                if (first == null) continue;
                if (!first.gameObject.activeInHierarchy || !first.IsInteractable()) continue;
                if (!BelongsTo(row, first.transform)) continue;
                events.SetSelectedGameObject(first.gameObject);
                return;
            }
        }
        catch (Exception exception)
        {
            Log.LogWarning("Could not focus the first Upscaling row: " + exception.Message);
        }
    }

    /// <summary>Re-reads the configuration into the cloned controls at a bounded rate.</summary>
    internal void RefreshIfVisible()
    {
        if (_disposed || !IsVisible) return;
        Refresh();
    }

    /// <summary>Writes every bound caption and value directly into its cached TMP component.</summary>
    internal void Refresh()
    {
        if (_disposed || _refreshing || Settings == null) return;
        _refreshing = true;
        try
        {
            var vr = VrState.Active;
            UpdateControlAvailability(vr);
            foreach (var binding in _bindings) SetText(binding.Caption, binding.CaptionText);

            SetText(_enabledRow.Value, Settings.Enabled.Value ? "On" : "Off");
            var upscaler = UpscalingModes.Name(Settings.Upscaler.Value);
            SetText(_upscalerRow.Value, upscaler);
            SetText(_modeRow.Value, UpscalingModes.Name(Settings.Preset.Value, Settings.Upscaler.Value));
            // VR has its own provider selection: the desktop choice stays remembered but is never what
            // this row shows or changes while a VR session owns the process.
            SetText(_frameGenerationRow.Value,
                vr ? UpscalingModes.Name(Settings.VrFrameGeneration.Value)
                   : UpscalingModes.Name(Settings.FrameGeneration.Value));
            SetText(_frameMultiplierRow.Value, FrameMultiplierLabel(vr));
            // The generic caption pass above must never win over the typed restart requirement.
            SetText(_frameGenerationRow.Caption, FrameGenerationCaption(vr));
        }
        catch (Exception exception)
        {
            Log.LogWarning("Could not refresh the Upscaling panel: " + exception.Message);
        }
        finally
        {
            _refreshing = false;
        }
    }

    /// <summary>
    /// The exact caption of the frame-generation row. The restart suffix comes only from the
    /// process family the native transport already claimed against the current selection, so it
    /// appears on the same key press that selects a provider of the other family, even with the
    /// settings page open and before any capture. It is suppressed in VR, where generation is
    /// intentionally off.
    /// </summary>
    private string FrameGenerationCaption(bool vr)
    {
        const string caption = "Frame Generation (Experimental)";
        var coordinator = Coordinator?.FrameGeneration;
        return !vr && coordinator != null && coordinator.RestartRequiredFor(RequestedFrameGeneration)
            ? caption + " [RESTART REQUIRED!]"
            : caption;
    }

    /// <summary>The provider the runtime would actually use for the current configuration.</summary>
    private FrameGenerationKind RequestedFrameGeneration => Settings.Enabled.Value
        ? UpscalingModes.Selectable(Settings.FrameGeneration.Value)
        : FrameGenerationKind.Off;

    /// <summary>
    /// Options the arrows may offer: the provider's proven maximum once it has run this session,
    /// and 2x until then. A provider that cannot be asked yet is never offered a total it may not
    /// produce, so FSR and a cold XeSS both stop at 2x. FSR legitimately has only 2x.
    /// </summary>
    private int FrameMultiplierCeiling(FrameGenerationKind provider) =>
        UpscalingModes.FrameMultiplierCeiling(
            provider, Coordinator?.FrameGeneration.ReportedMaximumMultiplier(provider) ?? 0);

    /// <summary>
    /// Displayed total: the remembered preference clamped to the proven maximum, so a value kept
    /// from another provider is shown as what the current provider will actually run.
    /// </summary>
    private string FrameMultiplierLabel(bool vr)
    {
        // VR frame generation is one real plus one generated image, always: the remembered desktop
        // total is neither shown nor clamped here, because it belongs to another provider.
        if (vr)
            return Settings.VrFrameGeneration.Value == FrameGenerationKind.Fsr ? "2x" : "Off";
        var provider = RequestedFrameGeneration;
        if (provider == FrameGenerationKind.Off) return "Off";
        var ceiling = FrameMultiplierCeiling(provider);
        if (ceiling < 2) return "Off";
        return Math.Clamp(Settings.FrameMultiplier.Value, 2, Math.Max(2, ceiling)).ToString() + "x";
    }

    /// <summary>
    /// Applies the stock disabled styling to the arrows the session must not accept: the multiplier in
    /// VR (fixed at 2x), the multiplier while generation is effectively off, and the multiplier while
    /// its provider has not proven a total above 2x. The provider row itself is always reachable.
    /// </summary>
    private void UpdateControlAvailability(bool vr)
    {
        SetArrowsInteractable(_frameGenerationRow, true);
        SetArrowsInteractable(_frameMultiplierRow,
            !vr && RequestedFrameGeneration != FrameGenerationKind.Off &&
            FrameMultiplierCeiling(RequestedFrameGeneration) > 2);
        if (vr == _vrLocked) return;
        _vrLocked = vr;
        RebuildNavigation();
    }

    private static void SetArrowsInteractable(RowBinding binding, bool interactable)
    {
        if (binding.Row.leftButton != null) binding.Row.leftButton.interactable = interactable;
        if (binding.Row.rightButton != null) binding.Row.rightButton.interactable = interactable;
    }

    /// <summary>Shared handler for the row arrows, used by both the buttons and the action hooks.</summary>
    private void HandleDirection(SettingsRow row, int direction)
    {
        if (row == null || _disposed) return;
        if (row == _enabledRow.Row) Toggle(Settings.Enabled, "Enabled");
        else if (row == _upscalerRow.Row) StepUpscaler(direction);
        else if (row == _modeRow.Row) StepMode(direction);
        else if (row == _frameGenerationRow.Row) StepFrameGeneration(direction);
        else if (row == _frameMultiplierRow.Row) StepFrameMultiplier(direction);
    }

    private void Toggle(BepInEx.Configuration.ConfigEntry<bool> entry, string name)
    {
        entry.Value = !entry.Value;
        Log.LogInfo($"panel: {name}={entry.Value}.");
        Refresh();
    }

    /// <summary>
    /// Cycles the fixed provider list, including providers this machine cannot run. The renderer
    /// then reports its exact Unsupported reason; the panel never substitutes another provider.
    /// </summary>
    private void StepUpscaler(int direction)
    {
        var order = UpscalingModes.UpscalerOrder;
        var index = Array.IndexOf(order, Settings.Upscaler.Value);
        if (index < 0) index = 0;
        index = (index + direction + order.Length) % order.Length;
        Settings.Upscaler.Value = order[index];
        Log.LogInfo($"panel: Upscaler={UpscalingModes.Name(order[index])}.");
        Refresh();
    }

    /// <summary>Cycles presets. Availability comes from the queried SDK ranges, not from this menu.</summary>
    private void StepMode(int direction)
    {
        var order = UpscalingModes.PresetOrder;
        var index = Array.IndexOf(order, Settings.Preset.Value);
        if (index < 0) index = 0;
        index = (index + direction + order.Length) % order.Length;
        Settings.Preset.Value = order[index];
        Log.LogInfo($"panel: Mode={UpscalingModes.Name(order[index], Settings.Upscaler.Value)}.");
        Refresh();
    }

    /// <summary>
    /// Cycles the independent frame-generation choice; Off is always reachable. In VR it toggles the
    /// VR-only provider between Off and AMD FSR, so a keyboard or controller hook can never change the
    /// remembered desktop provider from a VR session.
    /// </summary>
    private void StepFrameGeneration(int direction)
    {
        if (VrState.Active)
        {
            Settings.VrFrameGeneration.Value = Settings.VrFrameGeneration.Value == FrameGenerationKind.Fsr
                ? FrameGenerationKind.Off
                : FrameGenerationKind.Fsr;
            Log.LogInfo($"panel: VrFrameGeneration={UpscalingModes.Name(Settings.VrFrameGeneration.Value)}.");
            Refresh();
            return;
        }
        var order = UpscalingModes.FrameGenerationOrder;
        var index = Array.IndexOf(order, Settings.FrameGeneration.Value);
        if (index < 0) index = 0;
        index = (index + direction + order.Length) % order.Length;
        Settings.FrameGeneration.Value = order[index];
        Log.LogInfo($"panel: FrameGeneration={UpscalingModes.Name(order[index])}.");
        Refresh();
    }

    /// <summary>
    /// Cycles the remembered total multiplier over the totals this provider has proven it can
    /// produce. The write is refused in VR (the compositor path is fixed at 2x), while generation is
    /// off, and while the provider's proven maximum is 2x, so a 2x-only provider never overwrites the
    /// remembered preference.
    /// </summary>
    private void StepFrameMultiplier(int direction)
    {
        if (VrState.Active) return;
        var provider = RequestedFrameGeneration;
        var ceiling = FrameMultiplierCeiling(provider);
        if (ceiling <= 2) return;
        var options = ceiling - 1;
        var current = Math.Clamp(Settings.FrameMultiplier.Value, 2, ceiling);
        var index = (current - 2 + direction + options) % options;
        var value = 2 + index;
        Settings.FrameMultiplier.Value = value;
        Log.LogInfo($"panel: FrameMultiplier={value}x (requested {UpscalingModes.Name(provider)}, ceiling {ceiling}x).");
        Refresh();
    }

    /// <summary>True when this row belongs to this exact panel instance.</summary>
    private bool Owns(SettingsRow row)
    {
        foreach (var candidate in _all)
            if (candidate == row) return true;
        return false;
    }

    /// <summary>True when this row belongs to a Upscaling panel instance.</summary>
    internal static bool TryHandle(SettingsRow row, out SettingsPanel panel)
    {
        panel = null!;
        if (row == null) return false;
        if (!PanelsByRow.TryGetValue(row.GetInstanceID(), out var candidate) || candidate == null) return false;
        if (candidate._disposed || !candidate.Owns(row)) return false;
        panel = candidate;
        return true;
    }

    /// <summary>Removes this panel's cloned page, button, listeners and registry entries.</summary>
    internal void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        try
        {
            ReleaseCategoryColumn(true);
            foreach (var row in _all)
            {
                if (row == null) continue;
                var rowId = row.GetInstanceID();
                if (PanelsByRow.TryGetValue(rowId, out var owner) && owner == this) PanelsByRow.Remove(rowId);
                ClearListeners(row.leftButton);
                ClearListeners(row.rightButton);
                ClearListeners(row.slider);
            }
            if (_catagory != null) UnityEngine.Object.Destroy(_catagory.gameObject);
            if (_cloneRoot != null) UnityEngine.Object.Destroy(_cloneRoot);
        }
        catch (Exception exception)
        {
            Log.LogWarning("Could not fully dispose the Upscaling panel: " + exception.Message);
        }
        if (_menu != null)
        {
            var id = _menu.GetInstanceID();
            // A panel that failed staged construction never reached the registry; its rollback must
            // keep the stock Start observation so the next normal OnEnable can build again.
            var registered = PanelsByMenu.TryGetValue(id, out var owner) && owner == this;
            if (registered)
            {
                PanelsByMenu.Remove(id);
                if (StartedMenus.TryGetValue(id, out var started) && started == _menu) StartedMenus.Remove(id);
                BuildFailures.Remove(id);
            }
        }
        foreach (var pair in new List<KeyValuePair<int, SettingsPanel>>(PanelsByHost))
            if (pair.Value == this) PanelsByHost.Remove(pair.Key);
    }

    /// <summary>Hides every panel for an ordinary settings close; nothing is destroyed.</summary>
    internal static void HideAll()
    {
        foreach (var panel in new List<SettingsPanel>(PanelsByMenu.Values))
            if (panel != null && !panel._disposed) panel.Hide();
    }

    /// <summary>Disposes every panel. Used on plugin unload only.</summary>
    internal static void DisposeAll()
    {
        var panels = new List<SettingsPanel>(PanelsByMenu.Values);
        PanelsByMenu.Clear();
        foreach (var panel in panels) panel?.Dispose();
        PanelsByHost.Clear();
        PanelsByRow.Clear();
        StartedMenus.Clear();
        BuildFailures.Clear();
    }

    /// <summary>Disposes the panel registered for a host component.</summary>
    private static void DisposeHost(int hostId)
    {
        if (!PanelsByHost.Remove(hostId, out var panel) || panel == null) return;
        panel.Dispose();
    }

    /// <summary>Refreshes the panels whose page is on screen.</summary>
    internal static void RefreshVisible()
    {
        // No panel has ever been built: skip the snapshot copy, which would otherwise allocate a list
        // every time the game refreshes its settings screen.
        if (PanelsByMenu.Count == 0) return;
        foreach (var panel in new List<SettingsPanel>(PanelsByMenu.Values))
        {
            if (panel == null) continue;
            try
            {
                panel.RefreshIfVisible();
            }
            catch (Exception exception)
            {
                Log.LogWarning("Could not refresh a Upscaling panel: " + exception.Message);
            }
        }
    }

    /// <summary>Keeps the Upscaling tab in the game's own category list if a stock lifecycle rebuilt it.</summary>
    private void EnsureCategoryButtonListed()
    {
        if (_disposed || _categoryButton == null || _catagory == null) return;
        try
        {
            var list = _menu.buttonList;
            var source = _menu.catagoryGraphics != null ? _menu.catagoryGraphics.catagoryButton : null;
            var button = _categoryButton.GetComponent<Button>();
            if (list == null || source == null || button == null) return;
            if (_categoryButtonPointer == IntPtr.Zero) _categoryButtonPointer = button.Pointer;
            var pointer = _categoryButtonPointer;
            for (var i = 0; i < list.Count; i++)
            {
                var candidate = list[i];
                if (candidate != null && candidate.Pointer == pointer) return;
            }
            var sourcePointer = source.Pointer;
            var insertAt = list.Count;
            for (var i = 0; i < list.Count; i++)
            {
                var candidate = list[i];
                if (candidate == null || candidate.Pointer != sourcePointer) continue;
                insertAt = i + 1;
                break;
            }
            list.Insert(Mathf.Clamp(insertAt, 0, list.Count), button);
        }
        catch (Exception exception)
        {
            Log.LogWarning("Could not keep the Upscaling category tab in the button list: " + exception.Message);
        }
    }

    private void OnScreenEnabled()
    {
        if (_disposed) return;
        EnsureCategoryButtonListed();
        ApplyCategoryLayout();
        if (_localisationStripped) Refresh();
    }

    /// <summary>
    /// Harmony hooks that stop the game's settings storage from seeing custom rows and keep the
    /// panel in sync. Installed one at a time so an unsupported member disables only that
    /// interaction instead of the whole panel.
    /// </summary>
    internal static class Patches
    {
        // Lifecycle observers must run after the game's own method: a prefix would see fields that
        // the game has not populated yet.
        internal static void AfterStart(SettingsMenu __instance)
        {
            if (__instance == null) return;
            var id = __instance.GetInstanceID();
            StartedMenus[id] = __instance;
            BuildFailures.Remove(id);
            Ensure(__instance);
        }

        internal static void AfterEnable(SettingsMenu __instance)
        {
            if (__instance == null) return;
            var panel = ForMenu(__instance);
            if (panel != null)
            {
                panel.OnScreenEnabled();
                return;
            }
            // A new open is a new lifecycle: retry a previously failed build, but only for a menu
            // whose own Start has run.
            var id = __instance.GetInstanceID();
            if (!StartedMenus.TryGetValue(id, out var started) || started != __instance) return;
            BuildFailures.Remove(id);
            Ensure(__instance);
        }

        internal static void AfterDisable(SettingsMenu __instance)
        {
            Plugin.Instance?.Menu?.SettingsClosed();
            var panel = ForMenu(__instance);
            if (panel != null) panel.Hide();
            else HideAll();
        }

        /// <summary>
        /// Keeps the cloned page in step with the stock category swap: when another category becomes
        /// active the Upscaling page is hidden, so it is never visible behind a different tab.
        /// </summary>
        internal static void AfterSwap(SettingsMenu __instance)
        {
            var panel = ForMenu(__instance);
            if (panel == null) return;
            if (__instance.activeCatagory == panel._catagory) return;
            panel.Hide();
        }

        internal static bool BeforeLeft(SettingsRow __instance)
        {
            if (!TryHandle(__instance, out var panel)) return true;
            panel.HandleDirection(__instance, -1);
            return false;
        }

        internal static bool BeforeRight(SettingsRow __instance)
        {
            if (!TryHandle(__instance, out var panel)) return true;
            panel.HandleDirection(__instance, 1);
            return false;
        }

        internal static bool BeforeSelect0(SettingsRow __instance) => !TryHandle(__instance, out _);

        internal static bool BeforeSelect(SettingsRow __instance, int __0)
        {
            if (!TryHandle(__instance, out var panel)) return true;
            if (__0 == 0) panel.HandleDirection(__instance, 1);
            return false;
        }

        internal static bool BeforeCycle(SettingsRow __instance, int __0)
        {
            if (!TryHandle(__instance, out var panel)) return true;
            panel.HandleDirection(__instance, __0 < 0 ? -1 : 1);
            return false;
        }

        internal static bool BeforeRefresh(SettingsRow __instance)
        {
            if (!TryHandle(__instance, out var panel)) return true;
            panel.Refresh();
            return false;
        }

        /// <summary>Installs every hook, reporting each failure individually.</summary>
        internal static int Install(Harmony harmony, Action<string> log)
        {
            var installed = 0;
            installed += Patch(harmony, typeof(SettingsMenu), "Start", nameof(AfterStart), null, log, postfix: true);
            installed += Patch(harmony, typeof(SettingsMenu), "OnEnable", nameof(AfterEnable), null, log,
                postfix: true);
            installed += Patch(harmony, typeof(SettingsMenu), "OnDisable", nameof(AfterDisable), null, log,
                postfix: true);
            installed += Patch(harmony, typeof(SettingsMenu), "SwapToCatagory", nameof(AfterSwap),
                new[] { typeof(SettingsCatagory) }, log, postfix: true);
            installed += Patch(harmony, typeof(SettingsRow), "ActionLeft", nameof(BeforeLeft), null, log);
            installed += Patch(harmony, typeof(SettingsRow), "ActionRight", nameof(BeforeRight), null, log);
            installed += Patch(harmony, typeof(SettingsRow), "ActionSelect0", nameof(BeforeSelect0), null, log);
            installed += Patch(harmony, typeof(SettingsRow), "ActionSelect", nameof(BeforeSelect),
                new[] { typeof(int) }, log);
            installed += Patch(harmony, typeof(SettingsRow), "ActionCycle", nameof(BeforeCycle),
                new[] { typeof(int) }, log);
            installed += Patch(harmony, typeof(SettingsRow), "Refresh", nameof(BeforeRefresh), null, log);
            return installed;
        }

        private static int Patch(Harmony harmony, Type type, string method, string patchName, Type[]? signature,
            Action<string> log, bool postfix = false)
        {
            try
            {
                var target = signature == null
                    ? AccessTools.Method(type, method)
                    : AccessTools.Method(type, method, signature);
                if (target == null)
                {
                    log($"settings hook {type.Name}.{method}: method not found, that interaction stays native.");
                    return 0;
                }
                var patch = new HarmonyMethod(typeof(Patches), patchName);
                harmony.Patch(target, prefix: postfix ? null : patch, postfix: postfix ? patch : null);
                return 1;
            }
            catch (Exception exception)
            {
                log($"settings hook {type.Name}.{method} unavailable ({exception.GetType().Name}): " +
                    exception.Message);
                return 0;
            }
        }
    }

    /// <summary>
    /// Marks a menu instance as already handled. It deliberately carries no managed field, so the
    /// IL2CPP class injector only has to register a field-less MonoBehaviour; the panel itself lives
    /// in a static registry keyed by this component.
    /// </summary>
    internal sealed class PanelHost : MonoBehaviour
    {
        private void OnDestroy()
        {
            try
            {
                DisposeHost(GetInstanceID());
            }
            catch
            {
                // Unity is tearing the object down.
            }
        }
    }
}
