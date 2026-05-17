// =============================================================================
// SignalClearSRAP_YellowLine_v0_8.cpp
// Sierra Chart ACSIL Custom Study
//
// PURPOSE:
//   Rolling structural anchor / retail trap pivot line with delta labels
//   and an internal ATR volatility band.
//
// DESIGN BASIS:
//   Built from first-principles behavioral analysis and real-data reverse
//   engineering. Does NOT copy or reproduce proprietary code.
//
//   Key empirical findings driving the design:
//     - SRAP transitions every 1-2 bars (median gap = 2 bars over 1,000 bars)
//       => NOT a frozen event-driven line. It is a ROLLING structural anchor.
//     - Delta window sum range: -278 to +325, mean ~41, stddev ~85
//     - ATR half-band range: 65-151 NQ pts (mean ~94.5 pts)
//     - Anchor typically sits 30-40 pts from close at transitions
//
// ARCHITECTURE:
//   Three anchor modes via AnchorMode input:
//     0 = ZZ Reversal Price  (built-in Sierra Chart ZigZag, read via GetStudyArrayUsingID)
//     1 = MicroPOC            (existing MicroVPOC study, read via GetStudyArrayUsingID)
//     2 = Hybrid              (ZZ base; snap to POC if within proximity)
//
//   Each bar:
//     1. Read external ZZ reversal price (most recent non-zero lookback)
//     2. Read external MicroPOC price
//     3. Select anchor by AnchorMode
//     4. Compute rolling delta window sum (AskVolume - BidVolume over N bars)
//     5. Compute approximate ATR (SMA of True Range over ATRLength bars)
//     6. Compute upper/lower bands: Anchor +/- k * ATR
//     7. Write subgraph arrays (for spreadsheet / debug)
//     8. If anchor changed by >= UpdateThresholdTicks: redraw yellow line + label
//
// OUTPUTS:
//   SG0: SRAP Anchor Price     (IGNORE - drawn via UseTool as yellow dash)
//   SG1: Rolling Delta Sum     (IGNORE - value written for spreadsheet export)
//   SG2: Upper ATR Band        (LINE - optional, yellow/dim)
//   SG3: Lower ATR Band        (LINE - optional, yellow/dim)
//   SG4: DBG Anchor Price      (IGNORE - enable for validation)
//   SG5: DBG ATR Value         (IGNORE - enable to verify band width)
//   SG6: DBG Anchor Changed    (IGNORE - 1.0 when redraw fired)
//   SG7: DBG Anchor Source     (IGNORE - 0=ZZ, 1=POC, 2=HybridZZ, 3=HybridPOC)
//
// REQUIRES:
//   - sierrachart.h
//   - An existing Sierra Chart built-in ZigZag study on the same chart (AnchorMode 0 or 2)
//   - An existing SignalClear MicroVPOC study on same chart (AnchorMode 1 or 2)
//   - Intraday chart with bid/ask volume data (sc.AskVolume, sc.BidVolume)
// =============================================================================

#include "sierrachart.h"
#include <cmath>
#include <cstdio>

SCDLLName("SignalClear SRAP Yellow Line v0.8")


// =============================================================================
// SECTION 1: Helper — FindMostRecentNonZero
//
// Scans backward from CurrentIndex to find the most recent non-zero value in
// an array. Used to retrieve the last valid ZigZag reversal price, since the
// built-in ZZ Reversal Price subgraph is non-zero ONLY at actual swing pivots
// and zero on all other bars.
//
// FIX (audit issue #1 — HIGH):
//   CurrentIndex may exceed the external array size during load/recalc
//   mismatches (e.g. external study hasn't finished recalculating yet).
//   Starting index is clamped to min(CurrentIndex, ArraySize-1) before
//   the loop begins. Empty array returns 0.0f immediately.
// =============================================================================
static float FindMostRecentNonZero(
    SCFloatArray& Arr,
    int           CurrentIndex,
    int           LookbackMax)
{
    const int ArrSize = Arr.GetArraySize();
    if (ArrSize <= 0)
        return 0.0f;

    // Clamp start to valid array range — prevents OOB if external study
    // array is shorter than the current chart bar count
    int Start = CurrentIndex;
    if (Start >= ArrSize)
        Start = ArrSize - 1;

    int Limit = Start - LookbackMax;
    if (Limit < 0) Limit = 0;

    for (int i = Start; i >= Limit; --i)
    {
        if (Arr[i] != 0.0f)
            return Arr[i];
    }
    return 0.0f;
}


// =============================================================================
// SECTION 2: Helper — ComputeRollingDelta
//
// Returns the net ask-minus-bid volume sum over the last WindowBars closed bars.
// Positive = net buying pressure. Negative = net selling pressure.
// This is the "Δ" value displayed next to the yellow SRAP line.
//
// Observed real-data range: -278 to +325 over 8-bar windows on NQ 20R.
// Mean ~41, median ~32, stddev ~85.
// Heavy threshold: abs(delta) > 100 occurs ~23% of bars.
// =============================================================================
static float ComputeRollingDelta(
    SCStudyInterfaceRef sc,
    int                 CurrentIndex,
    int                 WindowBars)
{
    float Sum  = 0.0f;
    int   Start = CurrentIndex - WindowBars + 1;
    if (Start < 0) Start = 0;

    for (int i = Start; i <= CurrentIndex; ++i)
    {
        Sum += static_cast<float>(sc.AskVolume[i])
             - static_cast<float>(sc.BidVolume[i]);
    }
    return Sum;
}


// =============================================================================
// SECTION 3: Helper — ComputeApproxATR
//
// Approximates ATR using SMA of True Range on the current chart's bars.
// True Range = max(High-Low, |High-PrevClose|, |Low-PrevClose|)
//
// NOTE on NQ 20-Range bars:
//   High-Low is ALWAYS exactly 20 pts (the range is fixed by definition).
//   Gap components (|High-PrevClose|, |Low-PrevClose|) add to TR on gapped bars.
//   Typical SMA(TR, 24) on NQ 20R ≈ 20-25 pts per bar.
//
// The real SRAP study uses a 5-minute ATR(24, SMA) on a separate timeframe chart
// where ATR ≈ 25-35 pts typically, yielding half-bands of 65-151 NQ pts.
// To match that with current-chart approximation, use ATRMultiplier ≈ 4.0-5.0.
// Default: k = 4.5 → half-band ≈ 4.5 × 22 ≈ 99 pts (center of observed range).
// =============================================================================
static float ComputeApproxATR(
    SCStudyInterfaceRef sc,
    int                 CurrentIndex,
    int                 ATRLength)
{
    if (CurrentIndex < 1) return 0.0f;

    double TRSum  = 0.0;
    int    Count  = 0;
    int    Start  = CurrentIndex - ATRLength + 1;
    if (Start < 1) Start = 1; // Need prior close for TR

    for (int i = Start; i <= CurrentIndex; ++i)
    {
        const float PrevClose = sc.Close[i - 1];
        const float HiLo      = sc.High[i] - sc.Low[i];
        const float HiPC      = std::fabs(sc.High[i] - PrevClose);
        const float LoPC      = std::fabs(sc.Low[i]  - PrevClose);

        float TR = HiLo;
        if (HiPC > TR) TR = HiPC;
        if (LoPC > TR) TR = LoPC;

        TRSum += TR;
        ++Count;
    }

    return (Count > 0) ? static_cast<float>(TRSum / Count) : 0.0f;
}


// =============================================================================
// SECTION 3b: Experimental Anchor Helpers (v0.8)
//
// Four anchor candidates to test against the real SRAP. Each computes a
// different rolling control price over the last WindowBars bars.
// Compare outputs against real SRAP via SG9 (distance subgraph).
// =============================================================================

// Mode 3: Rolling VWAP — volume-weighted average close over last N bars.
// Stable, volume-aware, sits inside structure. Strong candidate.
static float ComputeRollingVWAP(SCStudyInterfaceRef sc, int CurrentIndex, int Window)
{
    double PVSum = 0.0, VSum = 0.0;
    int Start = CurrentIndex - Window + 1;
    if (Start < 0) Start = 0;
    for (int i = Start; i <= CurrentIndex; ++i)
    {
        double V = sc.Volume[i];
        PVSum += sc.Close[i] * V;
        VSum  += V;
    }
    return (VSum > 0.0) ? static_cast<float>(PVSum / VSum) : 0.0f;
}

// Mode 4: Rolling Volume-Weighted Midpoint — VWAP of bar midpoints (H+L)/2.
// Less close-biased than rolling VWAP, sits at price centre of gravity.
static float ComputeRollingVWMidpoint(SCStudyInterfaceRef sc, int CurrentIndex, int Window)
{
    double PVSum = 0.0, VSum = 0.0;
    int Start = CurrentIndex - Window + 1;
    if (Start < 0) Start = 0;
    for (int i = Start; i <= CurrentIndex; ++i)
    {
        double V   = sc.Volume[i];
        double Mid = (sc.High[i] + sc.Low[i]) * 0.5;
        PVSum += Mid * V;
        VSum  += V;
    }
    return (VSum > 0.0) ? static_cast<float>(PVSum / VSum) : 0.0f;
}

// Mode 5: Rolling High/Low Midpoint — simple mean of rolling high and low.
// No volume weighting. Tests whether pure price structure midpoint tracks SRAP.
static float ComputeRollingHLMidpoint(SCStudyInterfaceRef sc, int CurrentIndex, int Window)
{
    float RHigh = sc.High[CurrentIndex];
    float RLow  = sc.Low[CurrentIndex];
    int Start = CurrentIndex - Window + 1;
    if (Start < 0) Start = 0;
    for (int i = Start; i <= CurrentIndex; ++i)
    {
        if (sc.High[i] > RHigh) RHigh = sc.High[i];
        if (sc.Low[i]  < RLow)  RLow  = sc.Low[i];
    }
    return (RHigh + RLow) * 0.5f;
}

// Mode 6: Session VWAP — cumulative from session start to current bar.
// Uses sc.Subgraph[8].Arrays[0/1] as running accumulators.
// Arrays[0] = cumulative Price*Volume, Arrays[1] = cumulative Volume.
// Resets when a new trading day starts.
static float ComputeSessionVWAP(SCStudyInterfaceRef sc, int CurrentIndex)
{
    // Detect session boundary: new date vs prior bar
    bool NewSession = false;
    if (CurrentIndex == 0)
    {
        NewSession = true;
    }
    else
    {
        SCDateTime CurDT  = sc.BaseDateTimeIn[CurrentIndex];
        SCDateTime PrevDT = sc.BaseDateTimeIn[CurrentIndex - 1];
        NewSession = (CurDT.GetDate() != PrevDT.GetDate());
    }

    double& CumPV  = reinterpret_cast<double&>(sc.Subgraph[8].Arrays[0][CurrentIndex]);
    double& CumVol = reinterpret_cast<double&>(sc.Subgraph[8].Arrays[1][CurrentIndex]);

    if (NewSession || CurrentIndex == 0)
    {
        CumPV  = sc.Close[CurrentIndex] * sc.Volume[CurrentIndex];
        CumVol = sc.Volume[CurrentIndex];
    }
    else
    {
        double& PrevPV  = reinterpret_cast<double&>(sc.Subgraph[8].Arrays[0][CurrentIndex - 1]);
        double& PrevVol = reinterpret_cast<double&>(sc.Subgraph[8].Arrays[1][CurrentIndex - 1]);
        CumPV  = PrevPV  + sc.Close[CurrentIndex] * sc.Volume[CurrentIndex];
        CumVol = PrevVol + sc.Volume[CurrentIndex];
    }

    return (CumVol > 0.0) ? static_cast<float>(CumPV / CumVol) : 0.0f;
}


// Mode 7: Anchored VWAP — VWAP accumulated from a dynamic anchor bar.
// Anchor resets when the specified trigger fires (AVWAPResetMode input).
// Uses sc.Subgraph[9].Arrays[0/1] as persistent PV/V accumulators.
//
// ResetMode 0 = ZZ reversal bar — filtered by AccumVol threshold when enabled
// ResetMode 1 = Delta window sum sign flip vs prior bar
// ResetMode 2 = External reset signal study (non-zero = reset)
//
// AccumVolArray: ID2.SG5 (ZZ accumulated volume per swing)
// MinAccumVol:   minimum |AccumVol| to qualify as a significant rotation
// UseAccumFilter: when false, any ZZ pivot fires (v0.8 behavior)
static float ComputeAVWAP(
    SCStudyInterfaceRef sc,
    int                 Idx,
    int                 ResetMode,
    SCFloatArray&       ZZArray,
    SCFloatArray&       ExtResetArray,
    float               PriorDelta,
    float               CurrentDelta,
    SCFloatArray&       AccumVolArray,
    float               MinAccumVol,
    bool                UseAccumFilter)
{
    bool Reset = false;

    if (ResetMode == 0) // ZZ reversal — optionally filtered by AccumVol magnitude
    {
        bool ZZFired = (ZZArray.GetArraySize() > 0 && Idx < ZZArray.GetArraySize()
                        && ZZArray[Idx] != 0.0f);
        if (ZZFired && UseAccumFilter)
        {
            // Require |AccumVol| >= MinAccumVol to confirm significant rotation.
            // "They anchor to the largest player of the day" — small rotations skip.
            bool AccumOK = (AccumVolArray.GetArraySize() > 0
                            && Idx < AccumVolArray.GetArraySize()
                            && std::fabs(AccumVolArray[Idx]) >= MinAccumVol);
            Reset = AccumOK;
        }
        else
        {
            Reset = ZZFired; // No filter: any ZZ pivot resets (v0.8 behavior)
        }
    }
    else if (ResetMode == 1) // Delta sign flip
    {
        Reset = (Idx > 0)
             && ((PriorDelta >= 0.0f && CurrentDelta < 0.0f)
              || (PriorDelta <= 0.0f && CurrentDelta > 0.0f));
    }
    else if (ResetMode == 2) // External reset signal
    {
        Reset = (ExtResetArray.GetArraySize() > 0
                 && Idx < ExtResetArray.GetArraySize()
                 && ExtResetArray[Idx] != 0.0f);
    }

    double PrevPV  = (Idx > 0) ? sc.Subgraph[9].Arrays[0][Idx - 1] : 0.0;
    double PrevVol = (Idx > 0) ? sc.Subgraph[9].Arrays[1][Idx - 1] : 0.0;

    double CumPV, CumVol;
    if (Reset || Idx == 0)
    {
        CumPV  = sc.Close[Idx] * sc.Volume[Idx];
        CumVol = sc.Volume[Idx];
    }
    else
    {
        CumPV  = PrevPV  + sc.Close[Idx] * sc.Volume[Idx];
        CumVol = PrevVol + sc.Volume[Idx];
    }

    sc.Subgraph[9].Arrays[0][Idx] = static_cast<float>(CumPV);
    sc.Subgraph[9].Arrays[1][Idx] = static_cast<float>(CumVol);

    return (CumVol > 0.0) ? static_cast<float>(CumPV / CumVol) : 0.0f;
}


// =============================================================================
// SECTION 4: Main Study Function
// =============================================================================
SCSFExport scsf_SignalClearSRAP_YellowLine_v0_8(SCStudyInterfaceRef sc)
{
    // ── Subgraph references ───────────────────────────────────────────────────
    SCSubgraphRef SG_SRAPAnchor  = sc.Subgraph[0]; // Yellow line anchor price
    SCSubgraphRef SG_DeltaSum    = sc.Subgraph[1]; // Rolling delta window sum
    SCSubgraphRef SG_UpperBand   = sc.Subgraph[2]; // Anchor + k*ATR
    SCSubgraphRef SG_LowerBand   = sc.Subgraph[3]; // Anchor - k*ATR
    SCSubgraphRef SG_DBG_Anchor  = sc.Subgraph[4]; // Debug: anchor price
    SCSubgraphRef SG_DBG_ATR     = sc.Subgraph[5]; // Debug: current ATR value
    SCSubgraphRef SG_DBG_Changed = sc.Subgraph[6]; // Debug: 1.0 on redraw bars
    SCSubgraphRef SG_DBG_Source  = sc.Subgraph[7]; // Debug: anchor source
    SCSubgraphRef SG_ExpAnchor   = sc.Subgraph[8]; // Experiment: computed anchor value
    SCSubgraphRef SG_SRAPDist    = sc.Subgraph[9]; // Experiment: distance from real SRAP

    // ── Input references ──────────────────────────────────────────────────────
    SCInputRef In_ZZStudyID          = sc.Input[0];
    SCInputRef In_ZZSubgraphIdx      = sc.Input[1];
    SCInputRef In_POCStudyID         = sc.Input[2];
    SCInputRef In_POCSubgraphIdx     = sc.Input[3];
    SCInputRef In_AnchorMode         = sc.Input[4];
    SCInputRef In_POCProximityTicks  = sc.Input[5];
    SCInputRef In_DeltaWindow        = sc.Input[6];
    SCInputRef In_MinDeltaAbs        = sc.Input[7];
    SCInputRef In_ATRLength          = sc.Input[8];
    SCInputRef In_ATRMultiplier      = sc.Input[9];
    SCInputRef In_UpdateThreshTicks  = sc.Input[10];
    SCInputRef In_ShowATRBand        = sc.Input[11];
    SCInputRef In_ShowDeltaLabel     = sc.Input[12];
    SCInputRef In_ShowDebug          = sc.Input[13];
    SCInputRef In_ZZLookbackBars     = sc.Input[14];


    // =========================================================================
    // SetDefaults block — runs once to configure study properties and defaults
    // =========================================================================
    if (sc.SetDefaults)
    {
        sc.GraphName        = "SignalClear SRAP Yellow Line v0.8";
        sc.StudyDescription =
            "Rolling structural anchor / retail trap pivot (SRAP) with delta labels "
            "and ATR band. Original first-principles implementation. "
            "Reads ZigZag and/or MicroPOC via GetStudyArrayUsingID.";

        sc.AutoLoop                  = 1;
        sc.GraphRegion               = 0;   // Price region
        sc.DrawZeros                 = 0;
        sc.MaintainVolumeAtPriceData = 0;
        sc.ScaleRangeType            = SCALE_SAMEASREGION;

        // ── Subgraph 0: SRAP anchor (drawn via UseTool, stored per bar) ──
        SG_SRAPAnchor.Name         = "SRAP Anchor";
        SG_SRAPAnchor.DrawStyle    = DRAWSTYLE_IGNORE;
        SG_SRAPAnchor.PrimaryColor = RGB(255, 220, 0);
        SG_SRAPAnchor.DrawZeros    = false;

        // ── Subgraph 1: Rolling delta ──
        SG_DeltaSum.Name           = "Rolling Delta (Window Sum)";
        SG_DeltaSum.DrawStyle      = DRAWSTYLE_IGNORE;
        SG_DeltaSum.DrawZeros      = false;

        // ── Subgraph 2: Upper ATR band ──
        SG_UpperBand.Name          = "Upper ATR Band";
        SG_UpperBand.DrawStyle     = DRAWSTYLE_LINE;
        SG_UpperBand.PrimaryColor  = RGB(180, 160, 40);
        SG_UpperBand.LineWidth     = 1;
        SG_UpperBand.DrawZeros     = false;

        // ── Subgraph 3: Lower ATR band ──
        SG_LowerBand.Name          = "Lower ATR Band";
        SG_LowerBand.DrawStyle     = DRAWSTYLE_LINE;
        SG_LowerBand.PrimaryColor  = RGB(180, 160, 40);
        SG_LowerBand.LineWidth     = 1;
        SG_LowerBand.DrawZeros     = false;

        // ── Debug subgraphs (IGNORE by default; user enables in subgraph settings) ──
        SG_DBG_Anchor.Name         = "DBG: Anchor Price";
        SG_DBG_Anchor.DrawStyle    = DRAWSTYLE_IGNORE;
        SG_DBG_Anchor.DrawZeros    = false;

        SG_DBG_ATR.Name            = "DBG: ATR Value";
        SG_DBG_ATR.DrawStyle       = DRAWSTYLE_IGNORE;
        SG_DBG_ATR.DrawZeros       = false;

        SG_DBG_Changed.Name        = "DBG: Anchor Changed Flag";
        SG_DBG_Changed.DrawStyle   = DRAWSTYLE_IGNORE;
        SG_DBG_Changed.DrawZeros   = false;

        SG_DBG_Source.Name         = "DBG: Anchor Source (0=ZZ 1=POC 2=Hyb 3-6=Exp)";
        SG_DBG_Source.DrawStyle    = DRAWSTYLE_IGNORE;
        SG_DBG_Source.DrawZeros    = false;

        // ── v0.8: Experiment anchor output ──
        SG_ExpAnchor.Name          = "EXP: Computed Anchor";
        SG_ExpAnchor.DrawStyle     = DRAWSTYLE_LINE;
        SG_ExpAnchor.PrimaryColor  = RGB(0, 200, 200); // Cyan — distinct from yellow
        SG_ExpAnchor.LineWidth     = 1;
        SG_ExpAnchor.DrawZeros     = false;

        // Distance from real SRAP (positive = our anchor is above real SRAP)
        // Use this to compare candidates. Target: small absolute value, stable sign.
        SG_SRAPDist.Name           = "EXP: Distance from Real SRAP (pts)";
        SG_SRAPDist.DrawStyle      = DRAWSTYLE_LINE;
        SG_SRAPDist.PrimaryColor   = RGB(255, 100, 0); // Orange
        SG_SRAPDist.LineWidth      = 1;
        SG_SRAPDist.DrawZeros      = false;

        // ── Inputs ────────────────────────────────────────────────────────
        In_ZZStudyID.Name          = "ZigZag Study ID (0 = disable)";
        In_ZZStudyID.SetInt(0);

        // Built-in SC ZigZag: SG1=ZZLine, SG2=TextLabels, SG3=ReversalPrice
        // SG3 in SC UI notation = subgraph index 2 in ACSIL (0-based)
        In_ZZSubgraphIdx.Name      = "ZigZag Reversal Price Subgraph Index (0-based, usually 2 for SG3)";
        In_ZZSubgraphIdx.SetInt(2);
        In_ZZSubgraphIdx.SetIntLimits(0, 20);

        In_POCStudyID.Name         = "MicroPOC Study ID (0 = disable)";
        In_POCStudyID.SetInt(0);

        In_POCSubgraphIdx.Name     = "MicroPOC Subgraph Index (0-based)";
        In_POCSubgraphIdx.SetInt(0);
        In_POCSubgraphIdx.SetIntLimits(0, 20);

        // Anchor mode:
        //   0 = ZZ Reversal Price only
        //   1 = MicroPOC only
        //   2 = Hybrid (ZZ base, snap to POC if within ProximityTicks)
        In_AnchorMode.Name         = "Anchor Mode: 0=ZZ  1=POC  2=Hybrid  3=RollingVWAP  4=VWMidpoint  5=HLMidpoint  6=SessionVWAP";
        In_AnchorMode.SetInt(3); // Default to RollingVWAP for v0.8 experiment
        In_AnchorMode.SetIntLimits(0, 7);

        // ── v0.8: Anchored VWAP inputs ─────────────────────────────────────
        // ResetMode: 0=ZZ reversal  1=Delta sign flip  2=External signal
        sc.Input[26].Name = "AVWAP Reset Mode (mode 7 only): 0=ZZ  1=DeltaFlip  2=External";
        sc.Input[26].SetInt(0);
        sc.Input[26].SetIntLimits(0, 2);

        // External reset signal study (used when AVWAPResetMode = 2)
        sc.Input[27].Name = "AVWAP External Reset Study ID (ResetMode=2 only)";
        sc.Input[27].SetInt(0);

        sc.Input[28].Name = "AVWAP External Reset Subgraph Index";
        sc.Input[28].SetInt(0);
        sc.Input[28].SetIntLimits(0, 20);

        // ── v0.8: AccumVol reset filter (mode 7, ResetMode 0 only) ────────
        // Filters ZZ pivots by accumulated swing volume so only dominant
        // rotations ("largest player of the day") trigger an AVWAP reset.
        sc.Input[29].Name = "Use AccumVol Filter (mode 7, ResetMode=0 only)";
        sc.Input[29].SetYesNo(1);

        // Study ID for ZZ accumulated volume — typically ID2, subgraph SG5.
        // ID2.SG5 = AccumulatedVolume (net delta accumulated over each ZZ swing).
        sc.Input[30].Name = "ZZ AccumVol Study ID (usually ID2)";
        sc.Input[30].SetInt(0);

        sc.Input[31].Name = "ZZ AccumVol Subgraph Index (0-based, SG5 = index 4)";
        sc.Input[31].SetInt(4);
        sc.Input[31].SetIntLimits(0, 20);

        // Minimum |AccumVol| to count as a significant rotation.
        // Observed dominant swings: V:858, V:1081, V:1209.
        // Start at 500 — catches dominant rotations, ignores micro-pivots.
        // Lower to 300 for more sensitivity. Raise to 800 for dominant only.
        sc.Input[32].Name = "AVWAP Min |AccumVol| threshold (default 500)";
        sc.Input[32].SetFloat(500.0f);

        // Hybrid snap distance: POC must be within this many ticks of ZZ anchor
        // Default 40 ticks × 0.25 = 10 NQ pts
        In_POCProximityTicks.Name  = "Hybrid Snap Distance (ticks): POC must be within this of ZZ";
        In_POCProximityTicks.SetInt(40);
        In_POCProximityTicks.SetIntLimits(1, 400);

        // Rolling delta window: number of bars to sum delta over
        // Real study uses a window that produces deltas of ±30-300 range
        // Default 8 bars produces reasonable signal on NQ 20R
        In_DeltaWindow.Name        = "Rolling Delta Window (bars)";
        In_DeltaWindow.SetInt(8);
        In_DeltaWindow.SetIntLimits(1, 200);

        // Min abs delta to show label at full brightness
        // Below threshold: label is dimmed (line still draws)
        // Observed: 23% of bars exceed 100, 51% exceed 50
        In_MinDeltaAbs.Name        = "Min Abs Delta for Full-Brightness Label";
        In_MinDeltaAbs.SetFloat(30.0f);

        // ATR source: external study (preferred) or internal approximation (fallback)
        //
        // Confirmed from real data: the SRAP band uses an external ATR study
        // (ID14 in the reference chartbook). Set ATRStudyID to that study's ID.
        // When ATRStudyID = 0, falls back to internal SMA of True Range.
        //
        // Confirmed k = 1.58 (tight range 1.51-1.61 across full session).
        // Half-band = 1.58 × ATR. ATR on NQ 20R bars ≈ 44-93 pts.
        sc.Input[21].Name = "ATR Study ID (0 = use internal approximation)";
        sc.Input[21].SetInt(0);

        sc.Input[22].Name = "ATR Subgraph Index (0-based, usually 0 for SG1)";
        sc.Input[22].SetInt(0);
        sc.Input[22].SetIntLimits(0, 20);

        // ATR period: only used when ATRStudyID = 0 (internal fallback)
        In_ATRLength.Name          = "ATR Period — internal fallback only (bars, SMA of TR)";
        In_ATRLength.SetInt(24);
        In_ATRLength.SetIntLimits(2, 500);

        // k confirmed from real-data reverse engineering: 1.58
        // Half-band = k × ATR. With ATR ≈ 44-93 pts → half-band ≈ 69-147 pts.
        // Matches observed range of 65-151 pts exactly.
        In_ATRMultiplier.Name      = "ATR Band Multiplier k (confirmed ~1.58 from real data)";
        In_ATRMultiplier.SetFloat(1.58f);

        // Minimum anchor shift required before redrawing the yellow line
        // Prevents visual flicker on tiny float precision changes
        // Default 4 ticks = 1 NQ point
        In_UpdateThreshTicks.Name  = "Update Threshold (ticks): min anchor shift to redraw";
        In_UpdateThreshTicks.SetInt(4);
        In_UpdateThreshTicks.SetIntLimits(0, 200);

        In_ShowATRBand.Name        = "Show ATR Bands (SG2/SG3)";
        In_ShowATRBand.SetYesNo(1);

        In_ShowDeltaLabel.Name     = "Show Delta Label on Yellow Line";
        In_ShowDeltaLabel.SetYesNo(1);

        // When enabled: DBG subgraphs (SG4-SG7) are written every bar
        // Enable in subgraph settings to visualise
        In_ShowDebug.Name          = "Enable Debug Subgraphs (SG4-SG7)";
        In_ShowDebug.SetYesNo(0);

        // How far back to scan for the last ZZ reversal price
        In_ZZLookbackBars.Name     = "ZigZag Lookback (bars to scan for last reversal price)";
        In_ZZLookbackBars.SetInt(200);
        In_ZZLookbackBars.SetIntLimits(5, 2000);

        // When Yes (default): redraw on every live bar update even if anchor
        // has not moved. Keeps line visually current. On very high-frequency
        // data feeds this adds minor overhead — set to No to redraw only on
        // genuine anchor changes.
        sc.Input[15].Name    = "Redraw on Every Live Bar Update";
        sc.Input[15].SetYesNo(1);

        // ── v0.8: Experiment inputs ────────────────────────────────────────
        // Window for rolling anchor calculations (modes 3/4/5)
        sc.Input[23].Name = "Anchor Experiment Window (bars, modes 3/4/5)";
        sc.Input[23].SetInt(20);
        sc.Input[23].SetIntLimits(2, 500);

        // Reference to the real SRAP study (ID21) for distance comparison.
        // Set to the real SRAP study ID. SG9 will show how far each anchor
        // candidate is from the real SRAP line.
        sc.Input[24].Name = "Real SRAP Study ID (for comparison, 0=disable)";
        sc.Input[24].SetInt(0);

        sc.Input[25].Name = "Real SRAP Subgraph Index (usually 0 for SG1)";
        sc.Input[25].SetInt(0);
        sc.Input[25].SetIntLimits(0, 20);
        // When off: v0.8 behavior — one rolling line updated in place.
        // When on:  each anchor change freezes the prior segment as a finite
        //           horizontal line and starts a new active segment.
        sc.Input[16].Name = "Show Historical Segments";
        sc.Input[16].SetYesNo(0);

        // Maximum number of frozen historical segments kept on chart.
        // When exceeded, the oldest segment line number is reused (ring buffer).
        // Equivalent to DeleteOldestWhenLimitExceeded = always.
        sc.Input[17].Name = "Max Historical Segments";
        sc.Input[17].SetInt(8);
        sc.Input[17].SetIntLimits(1, 50);

        // Segment end mode:
        //   0 = ExtendUntilNextAnchorChange - segment closes when anchor shifts
        //   1 = FixedLength - segment line ends after SegmentLengthBars bars
        sc.Input[18].Name = "Segment End Mode: 0=AnchorChange  1=FixedLength";
        sc.Input[18].SetInt(0);
        sc.Input[18].SetIntLimits(0, 1);

        // Used only when SegmentEndMode = 1 (FixedLength)
        sc.Input[19].Name = "Segment Length (bars, used when SegmentEndMode=1)";
        sc.Input[19].SetInt(50);
        sc.Input[19].SetIntLimits(1, 2000);

        // Whether to show delta labels on historical (frozen) segments.
        // Default No - labels on active line only, avoids chart clutter.
        sc.Input[20].Name = "Show Delta Labels on Historical Segments";
        sc.Input[20].SetYesNo(0);

        return;
    } // end SetDefaults


    // =========================================================================
    // SECTION 5: Persistent state
    //
    // sc.PersistVars survives across bar updates within the same chart session.
    // Used to track the last-drawn anchor price and the fixed line numbers.
    //
    // f1 = last drawn SRAP anchor price (to detect meaningful change)
    // i1 = UseTool LineNumber for the yellow SRAP horizontal line
    // i2 = UseTool LineNumber for the delta text label
    // =========================================================================
    // f1 = last drawn SRAP anchor price
    // i1 = active yellow line LineNumber
    // i2 = active delta label LineNumber
    // i3-i5 = misconfiguration warn flags (existing)
    // i6 = SegmentCounter: total historical segments created (ring buffer index)
    // i7 = CurrentSegmentStartBar: bar index where the current segment began
    float& LastDrawnAnchor        = sc.PersistVars->f1;
    int&   SRAPLineNum            = sc.PersistVars->i1;
    int&   LabelLineNum           = sc.PersistVars->i2;
    int&   SegmentCounter         = sc.PersistVars->i6;
    int&   CurrentSegmentStartBar = sc.PersistVars->i7;

    // Historical segment slots: LineBase + 20 + (SegmentCounter % MaxSegs)
    // Ring buffer — oldest slot is reused when MaxSegs is exceeded.

    if (sc.IsFullRecalculation && sc.Index == 0)
    {
        LastDrawnAnchor        = 0.0f;
        SegmentCounter         = 0;
        CurrentSegmentStartBar = 0;

        // Use *20 stride to leave room for up to 18 historical slots below
        const int Base  = 4000 + (sc.StudyGraphInstanceID % 100) * 80;
        SRAPLineNum     = Base + 1;
        LabelLineNum    = Base + 2;

        sc.DeleteACSILDrawingTool(SRAPLineNum);
        sc.DeleteACSILDrawingTool(LabelLineNum);
        // Wipe all possible historical segment slots on clean recalc
        for (int s = 0; s < 50; ++s)
            sc.DeleteACSILDrawingTool(Base + 20 + s);
    }

    if (SRAPLineNum == 0)
    {
        const int Base  = 4000 + (sc.StudyGraphInstanceID % 100) * 80;
        SRAPLineNum     = Base + 1;
        LabelLineNum    = Base + 2;
    }

    // LineBase is used to compute historical segment line numbers consistently
    const int LineBase = SRAPLineNum - 1;  // Base = SRAPLineNum - 1

    const int Idx = sc.Index;


    // =========================================================================
    // SECTION 6: ReadExternalAnchorInputs
    //
    // Read ZigZag reversal price and MicroPOC arrays from their respective
    // external studies via sc.GetStudyArrayUsingID.
    //
    // ZigZag Reversal Price:
    //   - Non-zero only at confirmed swing highs/lows
    //   - FindMostRecentNonZero() scans back to retrieve the last pivot
    //
    // MicroPOC:
    //   - May be zero on the current forming bar; use prior bar as fallback
    // =========================================================================
    SCFloatArray ZZArray;
    SCFloatArray POCArray;
    bool ZZAvail  = false;
    bool POCAvail = false;

    const int ZZStudyID  = In_ZZStudyID.GetInt();
    const int POCStudyID = In_POCStudyID.GetInt();

    // ── Fail-fast config sanity checks — placed here, before anchor selection
    // and before the FinalAnchor == 0 early return, so misconfigured IDs are
    // always caught regardless of anchor state.
    //
    // Log-once behavior: PersistVars i3/i4/i5 act as "already warned" flags.
    // Each warning fires exactly once until the config is corrected (which
    // triggers a full recalc that resets i3/i4/i5 to 0).
    int& WarnedZZ     = sc.PersistVars->i3;
    int& WarnedPOC    = sc.PersistVars->i4;
    int& WarnedHybrid = sc.PersistVars->i5;

    const int AnchorMode = In_AnchorMode.GetInt();

    if (AnchorMode == 0 && ZZStudyID == 0)
    {
        if (!WarnedZZ)
        {
            sc.AddMessageToLog(
                "SignalClear SRAP: AnchorMode=ZZ but ZigZag Study ID is 0. "
                "Set ZigZag Study ID to your built-in ZZ study.", 1);
            WarnedZZ = 1;
        }
        return;
    }
    if (AnchorMode == 1 && POCStudyID == 0)
    {
        if (!WarnedPOC)
        {
            sc.AddMessageToLog(
                "SignalClear SRAP: AnchorMode=MicroPOC but MicroPOC Study ID is 0. "
                "Set MicroPOC Study ID to your SignalClear MicroVPOC study ID.", 1);
            WarnedPOC = 1;
        }
        return;
    }
    if (AnchorMode == 2 && ZZStudyID == 0 && POCStudyID == 0)
    {
        if (!WarnedHybrid)
        {
            sc.AddMessageToLog(
                "SignalClear SRAP: AnchorMode=Hybrid but both Study IDs are 0. "
                "Set at least ZigZag Study ID.", 1);
            WarnedHybrid = 1;
        }
        return;
    }
    // Config is valid — clear any prior warnings so they re-fire if user
    // misconfigures again after a fix
    WarnedZZ = WarnedPOC = WarnedHybrid = 0;

    if (ZZStudyID > 0)
    {
        sc.GetStudyArrayUsingID(ZZStudyID, In_ZZSubgraphIdx.GetInt(), ZZArray);
        ZZAvail = (ZZArray.GetArraySize() > 0);
    }

    if (POCStudyID > 0)
    {
        sc.GetStudyArrayUsingID(POCStudyID, In_POCSubgraphIdx.GetInt(), POCArray);
        POCAvail = (POCArray.GetArraySize() > 0);
    }


    // =========================================================================
    // SECTION 6b: Pre-compute rolling delta
    //
    // Hoisted before anchor selection so AnchorMode 7 (AVWAP) can use the
    // current delta for reset-mode 1 (sign flip). Section 8 reuses this value.
    // =========================================================================
    const int   DeltaWindow = In_DeltaWindow.GetInt();
    const float DeltaSum    = ComputeRollingDelta(sc, Idx, DeltaWindow);


    // =========================================================================
    // SECTION 7: SelectAnchorByMode
    //
    // Find the candidate anchor prices from each available source, then
    // select the final anchor based on AnchorMode input.
    //
    // ZZ anchor: most recent non-zero reversal price (scanned backward)
    // POC anchor: current bar value; fallback to prior bar if zero
    // =========================================================================

    // -- ZZ anchor --
    float ZZAnchor = 0.0f;
    if (ZZAvail)
    {
        ZZAnchor = FindMostRecentNonZero(ZZArray, Idx, In_ZZLookbackBars.GetInt());
    }

    // -- POC anchor --
    float POCAnchor = 0.0f;
    if (POCAvail)
    {
        if (Idx < POCArray.GetArraySize() && POCArray[Idx] != 0.0f)
        {
            POCAnchor = POCArray[Idx];
        }
        else if (Idx > 0 && (Idx - 1) < POCArray.GetArraySize() && POCArray[Idx - 1] != 0.0f)
        {
            // Forming bar may be 0 — use prior bar's valid POC
            POCAnchor = POCArray[Idx - 1];
        }
    }

    // -- Final anchor selection --
    // AnchorMode already declared in the fail-fast block above
    const float ProxPoints  = static_cast<float>(In_POCProximityTicks.GetInt()) * sc.TickSize;
    const int   ExpWindow   = sc.Input[23].GetInt();
    float FinalAnchor       = 0.0f;
    float AnchorSource      = -1.0f;

    if (AnchorMode == 0)
    {
        FinalAnchor  = ZZAnchor;
        AnchorSource = 0.0f;
    }
    else if (AnchorMode == 1)
    {
        FinalAnchor  = POCAnchor;
        AnchorSource = 1.0f;
    }
    else if (AnchorMode == 2) // Hybrid ZZ + POC
    {
        if (ZZAnchor != 0.0f && POCAnchor != 0.0f &&
            std::fabs(POCAnchor - ZZAnchor) <= ProxPoints)
        {
            FinalAnchor  = POCAnchor;
            AnchorSource = 3.0f;
        }
        else if (ZZAnchor != 0.0f)
        {
            FinalAnchor  = ZZAnchor;
            AnchorSource = 2.0f;
        }
        else if (POCAnchor != 0.0f)
        {
            FinalAnchor  = POCAnchor;
            AnchorSource = 1.0f;
        }
    }
    else if (AnchorMode == 3) // Rolling VWAP
    {
        FinalAnchor  = ComputeRollingVWAP(sc, Idx, ExpWindow);
        AnchorSource = 3.0f;
    }
    else if (AnchorMode == 4) // Rolling Volume-Weighted Midpoint
    {
        FinalAnchor  = ComputeRollingVWMidpoint(sc, Idx, ExpWindow);
        AnchorSource = 4.0f;
    }
    else if (AnchorMode == 5) // Rolling High/Low Midpoint
    {
        FinalAnchor  = ComputeRollingHLMidpoint(sc, Idx, ExpWindow);
        AnchorSource = 5.0f;
    }
    else if (AnchorMode == 6) // Session VWAP
    {
        FinalAnchor  = ComputeSessionVWAP(sc, Idx);
        AnchorSource = 6.0f;
    }
    else if (AnchorMode == 7) // Anchored VWAP
    {
        const int   AVWAPResetMode  = sc.Input[26].GetInt();
        const int   ExtResetID      = sc.Input[27].GetInt();
        const int   ExtResetSG      = sc.Input[28].GetInt();
        const bool  UseAccumFilter  = sc.Input[29].GetYesNo();
        const int   AccumVolID      = sc.Input[30].GetInt();
        const int   AccumVolSG      = sc.Input[31].GetInt();
        const float MinAccumVol     = sc.Input[32].GetFloat();

        SCFloatArray ExtResetArr;
        if (AVWAPResetMode == 2 && ExtResetID > 0)
            sc.GetStudyArrayUsingID(ExtResetID, ExtResetSG, ExtResetArr);

        SCFloatArray AccumVolArr;
        if (UseAccumFilter && AccumVolID > 0)
            sc.GetStudyArrayUsingID(AccumVolID, AccumVolSG, AccumVolArr);

        const float PriorDelta = (Idx > 0) ? SG_DeltaSum[Idx - 1] : 0.0f;

        FinalAnchor  = ComputeAVWAP(sc, Idx, AVWAPResetMode,
                                    ZZArray, ExtResetArr,
                                    PriorDelta, DeltaSum,
                                    AccumVolArr, MinAccumVol, UseAccumFilter);
        AnchorSource = 7.0f;
    }

    // No anchor found: write zeros and return cleanly
    if (FinalAnchor == 0.0f)
    {
        SG_SRAPAnchor[Idx]  = 0.0f;
        SG_DeltaSum[Idx]    = 0.0f;
        SG_UpperBand[Idx]   = 0.0f;
        SG_LowerBand[Idx]   = 0.0f;
        SG_DBG_Anchor[Idx]  = 0.0f;
        SG_DBG_ATR[Idx]     = 0.0f;
        SG_DBG_Changed[Idx] = 0.0f;
        SG_DBG_Source[Idx]  = -1.0f;
        return;
    }


    // =========================================================================
    // SECTION 7b: Experiment outputs — SG8 and SG9
    //
    // SG8: The anchor value this mode produced. Plot alongside real SRAP line
    //      to visually compare alignment.
    // SG9: Distance from real SRAP (this anchor - real SRAP, in pts).
    //      Target: small absolute value with consistent sign.
    //      Positive = our anchor sits above real SRAP.
    //      Negative = our anchor sits below real SRAP.
    //      Use the spreadsheet export to compute mean/stddev across session.
    // =========================================================================
    SG_ExpAnchor[Idx] = FinalAnchor;

    const int   RealSRAPStudyID = sc.Input[24].GetInt();
    const int   RealSRAPSGIdx   = sc.Input[25].GetInt();
    if (RealSRAPStudyID > 0)
    {
        SCFloatArray RealSRAPArr;
        sc.GetStudyArrayUsingID(RealSRAPStudyID, RealSRAPSGIdx, RealSRAPArr);
        if (RealSRAPArr.GetArraySize() > 0)
        {
            int RIdx = Idx;
            if (RIdx >= RealSRAPArr.GetArraySize()) RIdx = RealSRAPArr.GetArraySize() - 1;
            const float RealSRAP = RealSRAPArr[RIdx];
            SG_SRAPDist[Idx] = (RealSRAP != 0.0f) ? (FinalAnchor - RealSRAP) : 0.0f;
        }
    }
    else
    {
        SG_SRAPDist[Idx] = 0.0f;
    }


    // =========================================================================
    // SECTION 8: ComputeRollingDelta
    //
    // DeltaSum was pre-computed in Section 6b (hoisted for AVWAP mode 1).
    // Nothing to recompute here — value is already available.
    // =========================================================================


    // =========================================================================
    // SECTION 9: ComputeOrReadATRBand
    //
    // Approximate the 5-minute ATR using current chart True Range SMA.
    // Compute upper and lower bands: Anchor +/- k × ATR.
    //
    // ATR source priority:
    //   1. External study (sc.Input[21] > 0): read via GetStudyArrayUsingID.
    //      Confirmed source: ID14 in reference chartbook.
    //      ATR values on NQ 20R: 44-93 pts. k=1.58 → half-band 69-147 pts.
    //   2. Internal fallback (sc.Input[21] == 0): SMA of True Range on
    //      current chart bars. Requires higher k (~4.5) to match band widths.
    // =========================================================================
    const int   ATRStudyID  = sc.Input[21].GetInt();
    const int   ATRSGIdx    = sc.Input[22].GetInt();
    const float k           = In_ATRMultiplier.GetFloat();
    float       ATRVal      = 0.0f;

    if (ATRStudyID > 0)
    {
        // Read ATR from external study (ID14 or user-specified)
        SCFloatArray ATRArray;
        sc.GetStudyArrayUsingID(ATRStudyID, ATRSGIdx, ATRArray);
        if (ATRArray.GetArraySize() > 0)
        {
            int ATRIdx = Idx;
            if (ATRIdx >= ATRArray.GetArraySize())
                ATRIdx = ATRArray.GetArraySize() - 1;
            ATRVal = ATRArray[ATRIdx];
        }
        // If external read returned zero, fall through to internal fallback
        if (ATRVal == 0.0f)
            ATRVal = ComputeApproxATR(sc, Idx, In_ATRLength.GetInt());
    }
    else
    {
        // Internal fallback: SMA of True Range on current chart bars
        ATRVal = ComputeApproxATR(sc, Idx, In_ATRLength.GetInt());
    }

    const float UpperBand = FinalAnchor + k * ATRVal;
    const float LowerBand = FinalAnchor - k * ATRVal;


    // =========================================================================
    // SECTION 10: OutputDebugSubgraphs
    //
    // SG0-SG3: always written (operational + spreadsheet-ready).
    // SG4-SG7: ONLY written when ShowDebug == Yes.
    //          This matches the input's documented behavior.
    //          When ShowDebug is off, these subgraphs are zeroed each bar
    //          so stale values don't linger in the spreadsheet export.
    //
    // FIX (audit issue #2 — MEDIUM):
    //   Previously SG4-SG7 were always written regardless of ShowDebug.
    //   Now gated behind In_ShowDebug.GetYesNo().
    //
    // Validation checklist (ShowDebug = Yes):
    //   SG4 (Anchor):  should match ZZ reversal prices at swing pivots
    //   SG5 (ATR):     should be 44-93 pts when reading ID14 externally.
    //                  If reading internal fallback: ~20-25 pts on NQ 20R.
    //                  Verify: (SG2 - SG3) / 2 should equal k × SG5 = 1.58 × SG5.
    //   SG6 (Changed): spikes of 1.0 align with visible line moves
    //   SG7 (Source):  steady 0=ZZ mode, steady 1=POC mode,
    //                  alternating 2/3 = Hybrid switching on/off snap
    // =========================================================================
    SG_SRAPAnchor[Idx] = FinalAnchor;
    SG_DeltaSum[Idx]   = DeltaSum;

    if (In_ShowATRBand.GetYesNo())
    {
        SG_UpperBand[Idx] = UpperBand;
        SG_LowerBand[Idx] = LowerBand;
    }
    else
    {
        SG_UpperBand[Idx] = 0.0f;
        SG_LowerBand[Idx] = 0.0f;
    }

    if (In_ShowDebug.GetYesNo())
    {
        SG_DBG_Anchor[Idx] = FinalAnchor;
        SG_DBG_ATR[Idx]    = ATRVal;
        // SG6 (Changed) is written further below after ShouldDraw is determined
        SG_DBG_Source[Idx] = AnchorSource;
    }
    else
    {
        SG_DBG_Anchor[Idx]  = 0.0f;
        SG_DBG_ATR[Idx]     = 0.0f;
        SG_DBG_Changed[Idx] = 0.0f;
        SG_DBG_Source[Idx]  = 0.0f;
    }


    // =========================================================================
    // SECTION 11: RenderYellowLineAndDeltaLabel
    //
    // Two modes controlled by Input[16] (ShowHistoricalSegments):
    //
    // MODE A — ShowHistoricalSegments = No (v0.8 behavior):
    //   One rolling DRAWING_HORIZONTALLINE updated in place. Extends to right
    //   edge. Active line moves when anchor changes by >= UpdateThresholdTicks.
    //
    // MODE B — ShowHistoricalSegments = Yes (v0.8):
    //   Each anchor change freezes the prior segment as a DRAWING_LINE with
    //   explicit begin/end datetimes. A new active segment starts. Frozen
    //   segments are stored in a ring buffer of MaxHistoricalSegments slots.
    //   When the ring wraps, the oldest slot is overwritten (delete oldest).
    //
    //   Segment end behavior (Input[18]):
    //     0 = ExtendUntilNextAnchorChange — segment closes at anchor-change bar
    //     1 = FixedLength — segment closes after Input[19] bars from its start
    //
    //   Delta labels on historical segments: Input[20] (default No).
    // =========================================================================
    const float UpdateThreshPts =
        static_cast<float>(In_UpdateThreshTicks.GetInt()) * sc.TickSize;

    const bool AnchorChanged       = (std::fabs(FinalAnchor - LastDrawnAnchor) >= UpdateThreshPts);
    const bool IsLastBar           = (Idx >= sc.ArraySize - 1);
    const bool LiveRedraw          = sc.Input[15].GetYesNo();
    const bool ShowHistSegments    = sc.Input[16].GetYesNo();
    const int  MaxSegs             = sc.Input[17].GetInt();
    const int  SegEndMode          = sc.Input[18].GetInt();
    const int  SegLengthBars       = sc.Input[19].GetInt();
    const bool ShowHistLabels      = sc.Input[20].GetYesNo();

    const bool ShouldDraw = AnchorChanged || (IsLastBar && LiveRedraw);

    if (In_ShowDebug.GetYesNo())
        SG_DBG_Changed[Idx] = ShouldDraw ? 1.0f : 0.0f;

    if (!ShouldDraw)
        return;

    // ── Lambda-style helper: draw or update a delta label ─────────────────
    // Written as inline block to avoid forward-declaration complexity in ACSIL.
    // Called for both the active label and optionally historical labels.
    auto DrawDeltaLabel = [&](int LNum, float AnchorPrice, SCDateTimeMS DateTime)
    {
        char Buf[32];
        if (DeltaSum >= 0.0f)
            snprintf(Buf, sizeof(Buf), "+%d", static_cast<int>(DeltaSum));
        else
            snprintf(Buf, sizeof(Buf), "%d",  static_cast<int>(DeltaSum));

        const COLORREF Col = (std::fabs(DeltaSum) >= In_MinDeltaAbs.GetFloat())
            ? RGB(255, 220, 0) : RGB(140, 120, 30);

        s_UseTool T;
        T.Clear();
        T.ChartNumber   = sc.ChartNumber;
        T.LineNumber    = LNum;
        T.DrawingType   = DRAWING_TEXT;
        T.AddMethod     = UTAM_ADD_OR_ADJUST;
        T.BeginDateTime = DateTime;
        T.Price1        = AnchorPrice + 3.0f * sc.TickSize;
        T.Text          = Buf;
        T.Color         = Col;
        T.FontSize      = 10;
        T.FontBold      = true;
        sc.UseTool(T);
    };

    // =========================================================================
    // MODE A: v0.8 rolling line (ShowHistoricalSegments = No)
    // =========================================================================
    if (!ShowHistSegments)
    {
        LastDrawnAnchor = FinalAnchor;

        s_UseTool LineTool;
        LineTool.Clear();
        LineTool.ChartNumber       = sc.ChartNumber;
        LineTool.LineNumber        = SRAPLineNum;
        LineTool.DrawingType       = DRAWING_HORIZONTALLINE;
        LineTool.AddMethod         = UTAM_ADD_OR_ADJUST;
        LineTool.BeginDateTime     = sc.BaseDateTimeIn[Idx];
        LineTool.Price1            = FinalAnchor;
        LineTool.Color             = RGB(255, 220, 0);
        LineTool.LineWidth         = 2;
        LineTool.LineStyle         = LINESTYLE_DASH;
        LineTool.TransparencyLevel = 0;
        sc.UseTool(LineTool);

        if (In_ShowDeltaLabel.GetYesNo())
            DrawDeltaLabel(LabelLineNum, FinalAnchor, sc.BaseDateTimeIn[Idx]);
        else
            sc.DeleteACSILDrawingTool(LabelLineNum);

        return;
    }

    // =========================================================================
    // MODE B: Historical Segment Mode (ShowHistoricalSegments = Yes)
    // =========================================================================

    if (AnchorChanged && LastDrawnAnchor != 0.0f)
    {
        // ── Freeze the current segment as a finite DRAWING_LINE ───────────
        // Slot in ring buffer: wraps at MaxSegs, overwrites oldest.
        const int SlotIdx    = SegmentCounter % MaxSegs;
        const int FrozenNum  = LineBase + 20 + SlotIdx;
        const int FrozenLabelNum = LineBase + 20 + 50 + SlotIdx; // label slots offset by 50

        // Determine end bar for this segment
        int EndBar = Idx;
        if (SegEndMode == 1) // FixedLength
        {
            int FixedEnd = CurrentSegmentStartBar + SegLengthBars;
            if (FixedEnd < Idx) EndBar = FixedEnd;
        }
        // Clamp to valid range
        if (EndBar >= sc.ArraySize) EndBar = sc.ArraySize - 1;
        if (EndBar < CurrentSegmentStartBar) EndBar = CurrentSegmentStartBar;

        // Draw frozen segment line: explicit begin and end datetimes
        {
            s_UseTool Seg;
            Seg.Clear();
            Seg.ChartNumber       = sc.ChartNumber;
            Seg.LineNumber        = FrozenNum;
            Seg.DrawingType       = DRAWING_LINE;
            Seg.AddMethod         = UTAM_ADD_OR_ADJUST;
            Seg.BeginDateTime     = sc.BaseDateTimeIn[CurrentSegmentStartBar];
            Seg.EndDateTime       = sc.BaseDateTimeIn[EndBar];
            Seg.Price1            = LastDrawnAnchor;
            Seg.Price2            = LastDrawnAnchor;
            Seg.Color             = RGB(200, 180, 40); // Dimmed yellow for historical
            Seg.LineWidth         = 1;
            Seg.LineStyle         = LINESTYLE_DASH;
            Seg.TransparencyLevel = 50;
            sc.UseTool(Seg);
        }

        // Historical delta label (optional)
        if (ShowHistLabels && In_ShowDeltaLabel.GetYesNo())
            DrawDeltaLabel(FrozenLabelNum, LastDrawnAnchor,
                           sc.BaseDateTimeIn[CurrentSegmentStartBar]);
        else
            sc.DeleteACSILDrawingTool(FrozenLabelNum);

        ++SegmentCounter;
        CurrentSegmentStartBar = Idx;
    }
    else if (LastDrawnAnchor == 0.0f)
    {
        // Very first segment — just record the start bar
        CurrentSegmentStartBar = Idx;
    }

    // ── Update the active (current) segment line ──────────────────────────
    // Active line: DRAWING_HORIZONTALLINE extending from segment start to
    // right edge. This visually "lives" until the next anchor change.
    LastDrawnAnchor = FinalAnchor;

    {
        s_UseTool ActiveLine;
        ActiveLine.Clear();
        ActiveLine.ChartNumber       = sc.ChartNumber;
        ActiveLine.LineNumber        = SRAPLineNum;
        ActiveLine.DrawingType       = DRAWING_HORIZONTALLINE;
        ActiveLine.AddMethod         = UTAM_ADD_OR_ADJUST;
        ActiveLine.BeginDateTime     = sc.BaseDateTimeIn[CurrentSegmentStartBar];
        ActiveLine.Price1            = FinalAnchor;
        ActiveLine.Color             = RGB(255, 220, 0);
        ActiveLine.LineWidth         = 2;
        ActiveLine.LineStyle         = LINESTYLE_DASH;
        ActiveLine.TransparencyLevel = 0;
        sc.UseTool(ActiveLine);
    }

    // Active delta label (always on active segment if ShowDeltaLabel = Yes)
    if (In_ShowDeltaLabel.GetYesNo())
        DrawDeltaLabel(LabelLineNum, FinalAnchor, sc.BaseDateTimeIn[Idx]);
    else
        sc.DeleteACSILDrawingTool(LabelLineNum);

    // End of scsf_SignalClearSRAP_YellowLine_v0_8
}
