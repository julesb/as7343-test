# Colour calibration — approaches & concepts

High-level notes for Phase 2: using the AS7343 to generate Quicksilver colour
profiles automatically, aiming to beat the hand-eyeballed profiles. Hobby-grade,
not metrology. No code yet — this is the thinking.

## 1. What we're actually producing (the target format)

Quicksilver's colour correction (`qsclient` `ColorProfile`) is **three
independent per-channel transfer curves** — one each for R, G, B:

- Each curve is a list of `{input, output}` control points in `0..1`,
  piecewise-linear, evaluated by interpolation.
- Applied per point as `r' = redCurve(r)`, `g' = greenCurve(g)`,
  `b' = blueCurve(b)`. **Channels never mix** — there is no 3×3 matrix, no
  cross-talk term, no brightness-dependent variant.
- Applied late in the pipeline, after geometry/transitions, just before the
  points are sent to the DAC. Blanking forces `0,0,0` separately.

What this buys us / limits us:

- We can reshape **each channel's own intensity response** as much as we like
  (gain, gamma/linearity, threshold lift).
- We **cannot** correct chromaticity by mixing channels, and we can't make the
  correction depend on overall brightness. We don't need to — the whole point of
  a good per-channel curve is that it makes brightness-dependence go away (§3).
- A profile is just JSON. Our tool's job is ultimately: **emit three lists of
  control points.** That's the entire output.

The existing hand profile `W3000A Refined 2` is a textbook example of the format
being used for two jobs at once:

```
red:   0→0,  0⁺→0.155, 0.40→0.38, 0.65→0.57, 1→1.0     (threshold lift + mild curve)
green: 0→0,  0⁺→0.069, 0.28→0.14, 0.79→0.37, 1→0.83    (pulled DOWN hard at top)
blue:  0→0,  0⁺→0.224, ... 0.81→0.80, 1→1.0            (big threshold lift)
```

Note the doubled point at input 0 (`0→0` then `0⁺→threshold`): true-off stays
black for blanking, but the smallest *positive* input jumps straight to the
laser's lasing threshold. That trick is worth keeping when we generate curves.

## 2. Why the eyeballed profiles don't hold across brightness

The lasers differ in three ways at once: **power** (R 0.6 W / G 0.9 W /
B 1.5 W), **lasing threshold** (drive level where light first appears), and
**response curve shape** (how light grows with drive — diode + galvo/DAC +
modulation are not linear). Green is also where the eye is most sensitive, so
0.9 W of 520 nm dominates perceptually.

If you balance white at *one* brightness (the usual eyeball method), you've
matched the three channels at a single point. Everywhere else the three curves
have different shapes and different thresholds, so the ratios drift and white
takes on a tint — exactly the "can't get it balanced at all levels" symptom.

The fix is not a better single balance point. It's making each channel's
**measured light output a known, matched function of input across the whole
range.** If all three channels are (say) linear in light vs input with the same
shape, then any grey `(v,v,v)` stays neutral at every `v`. That is the job the
sensor lets us do that the eye can't.

## 3. Core method: sweep → linearise → balance

Per channel, independently:

1. **Sweep** the laser drive value `0 → 1` in steps, one laser at a time, and
   record the sensor response at each step → the channel's *native* curve
   `light = f(drive)`.
2. **Find the threshold**: the lowest drive where light rises convincingly out
   of the noise. Below it the laser is effectively off.
3. **Invert** `f` to get the correction curve `drive = f⁻¹(target_light)`. Sample
   that inverse at a handful of inputs → those become the profile's control
   points. This is what linearises the channel: after correction, equal input
   steps produce equal light steps.
4. **Balance**: scale the three linearised channels so a full-white input lands
   on the chosen white ratio (§5). Because all three are now linear, that one
   scaling holds at *every* brightness — white tracks neutral all the way down.

The control-point list format is forgiving: we just sample the inverse curve at
~5–8 inputs and let Quicksilver's piecewise-linear interpolation fill in. Keep
the `0→0` plus `0⁺→threshold` pair at the bottom so blanking still goes black.

No heavy maths required: invert-by-table-lookup (for each desired output light,
find the drive that produced it in the sweep) is enough. No curve fitting needed.

## 4. Sensor bands — single band vs weighted sum (your question)

Each laser is narrowband but the AS7343 bands are broad and overlapping, so a
laser lands mostly on one band and leaks into neighbours:

| Laser      | Peak band     | Also lights        |
|------------|---------------|--------------------|
| R 638 nm   | F6 (640)      | FXL (600), F7 (690)|
| G 520 nm   | F4 (515)      | F5 (550), FY (555) |
| B 445 nm   | FZ (450)      | F2 (425), F3 (475) |

Recommendations:

- **For the per-channel sweep (linearisation), single dominant band is fine.**
  We only care that the measure is *monotonic and proportional* to that laser's
  output — absolute calibration doesn't matter. The peak band is simplest and
  least contaminated by the other lasers (which are off during the sweep anyway).
- **A weighted/simple sum of the 1–2 neighbour bands mainly helps robustness**:
  noise averaging, and immunity to the peak band saturating while neighbours
  don't. Low cost, mild benefit. Worth offering as an option, not essential.
- **Tidiest trick: use the Clear/VIS broadband channel for the sweep.** Since
  only one laser is on at a time, Clear *is* that laser's total flux — no band
  selection needed at all. (Clear is useless for measuring a *mixture*, so it's
  sweep-only.) Good candidate for the simplest first version.
- **Always subtract a dark/ambient baseline**: take a reading with the laser
  blanked and subtract it from every sweep sample. This is what makes the
  flexible/uncontrolled setup (§6) workable.
- Watch saturation/headroom using the existing `meta` analog/digital-sat flags
  and integration time; pick a gain/IT where full-brightness doesn't clip and
  the threshold region still reads above noise. May need a different gain per
  channel given the power spread.

## 5. The white-balance target (the one perceptual caveat)

"Equal sensor counts on R, G, B" is **not** the same as "looks neutral white" —
the AS7343 bands are not the eye's response, and the sensor sees the 1.5 W blue
and 0.6 W red very differently from how you do. So:

- Linearisation (§3 steps 1–3) is objective and the sensor does it well.
- The final **white ratio** (§3 step 4) is a *choice*, not a measurement. Options:
  - Pick a target ratio from luminous weighting (cheap approximation), or
  - **Leave a simple by-eye trim**: the tool linearises everything, then you nudge
    three master gains until white looks right *on your screen*. Because the
    channels are already linear, that trim stays correct at all brightnesses —
    which is the property the current profiles lack.
- This split keeps the maths-y part (linearisation) on the sensor and the
  subjective part (white point) on your eye, where it belongs.

## 6. Measurement setup — flexibility first

The method only needs the measurement to be **monotonic, repeatable, and
ambient-subtracted** during a sweep. It does *not* need a controlled lab. So:

- Starting loose (point at ceiling / project on the black dev screen) is fine for
  getting a feel. Calibrating off the **black screen you actually project on** is
  arguably *correct*, not a compromise — it folds in that screen's spectral
  absorption, so you're balancing the light you really see.
- Keep the sensor **fixed** for the duration of one full R/G/B sweep (movement
  between channels = bad balance). A flexible mount that you can clamp in place is
  enough; the box/pinhole/diffuser rig can come later for repeatability.
- The eventual `laser → pinhole → diffuser → sensor` box mainly buys: lower
  ambient, no specular hotspots, consistent geometry run-to-run. Nice-to-have,
  not a prerequisite.
- Practical guardrails the tool should enforce regardless of setup: dark-frame
  subtract, saturation check, and a "did the signal actually move?" sanity test
  per channel (catches a misaimed sensor before it produces a garbage curve).

## 7. Suggested staging

1. **v0 — manual-assist.** Firmware can already stream the sensor and take
   gain/IT over OSC. Add laser drive control, then a host script that sweeps one
   channel, prints `drive vs light`, and we eyeball the curve. Proves the
   measurement and the rig.
2. **v1 — auto per-channel linearise.** Automate sweep → threshold → invert →
   sample control points → write a Quicksilver `ColorProfile` JSON. Clear channel
   as the measure; dark-frame subtract. White ratio left as a by-eye trim.
3. **v2 — refinements if wanted.** Weighted band sums, perceptual white target,
   gain-per-channel auto-pick, repeatable box rig, saved raw sweeps for
   re-processing without re-measuring.

## 8. Open decisions

- Where does laser drive come from during calibration — does the tool drive the
  lasers through Quicksilver (OSC), or a dedicated calibration path? (Need to
  confirm how `qsclient` exposes per-channel output for a steady test colour.)
- White-point target: luminous-weighted vs pure by-eye trim (or both).
- Measure: Clear-only (simplest) vs peak band vs weighted neighbour sum.
- Profile authoring: write directly into `qsclient`'s `color_profiles.json`, or
  emit a file to import? (Affects whether the tool lives in this repo or qsclient.)

(Control-point count is no longer an open question — it's a calibration-run
parameter, see §9.)

## 9. Control-point count, fidelity, and runtime cost

Two distinct knobs, worth keeping separate in the tool:

- **Measurement density** — how finely we sweep the laser drive. Cheap; measure
  as densely as we like, it only improves the inverse. Not the same as the next.
- **Emitted control-point count** — how many points we distil the measured curve
  into for the profile. **This is the user parameter for a calibration run.**

On the emitted count:

- Fidelity is capped by the DAC, not the curve. The transfer function is smooth
  and monotonic, so past ~8–16 *well-placed* points the residual sits below
  12-bit quantisation — numerically invisible, because the output can't represent
  the difference. Beyond that, more points buy nothing real.
- Placement beats count. The points earn their keep at the **threshold bend near
  zero**; the smooth upper region barely needs any. So consider **adaptive
  placement** (pack points where curvature is high) to hit a fidelity target with
  fewer points, rather than a uniform count.
- So the parameter is best framed as *either* a target fidelity (tool picks the
  points) *or* an explicit count for manual control — not a value the user has to
  guess blindly.

Crucially, with the bake-to-LUT change below, control-point count has **no
runtime cost**, so this knob is purely about fidelity, file size, and
editability.

## 10. Quicksilver-side notes (separate work)

Surfaced during this investigation; not calibration-tool work, handled separately
in `qsclient`, but they shape the parameters above. Both stand on their own merits
regardless of calibration.

- **The curve evaluator is a linear scan.** `ColorCurve::evaluate()` walks the
  control-point list for every channel of every point — O(N) per channel. Fine at
  5–6 points, smelly as counts grow. A binary search (`std::lower_bound`) is a
  cheap, self-contained fix.
- **Bake the active profile into a per-channel LUT.** Better still: when a profile
  becomes active, pre-compute a per-channel LUT sized to that *output's* DAC
  resolution (e.g. 4096 entries for a 12-bit DAC, 256 for 8-bit). Runtime apply
  then becomes a single array index — O(1) — independent of control-point count.
  This decouples authoring (sparse, editable, resolution-independent control
  points; what the calibration tool emits) from applying (dense direct-index LUT;
  what the hardware wants), and is why §9's count knob can be free at runtime.
  Baking per output also lets one resolution-independent profile drive a 12-bit
  and an 8-bit DAC correctly.
