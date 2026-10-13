# 📐 6-Axis IMU, Spirit Level & Auto-Rotation (QMI8658)

TuneBar leverages the onboard **QMI8658 6-axis IMU** (3-axis accelerometer + 3-axis gyroscope) to provide jitter-free **Landscape Auto-Rotation** and a precision **3-Panel Digital Inclinometer & Spirit Level**.

---

## 🔄 Landscape Auto-Rotation Architecture

* **Strict Landscape Locking**: To preserve the ultra-wide $640\times172$ widescreen layout, rotation is restricted strictly to horizontal landscape modes (**$0^\circ$ Normal Landscape** $\leftrightarrow$ **$180^\circ$ Inverted Landscape**). Portrait orientations ($|A_x| > 0.75g$) and flat desk placements ($|A_z| > 0.85g$) are locked in place and ignored.
* **Physical Edge & Port Reference**:
  - **Normal Landscape**: Power buttons facing up, screen facing user.
  - **Left Edge Rest (Portrait CCW)**: Device resting on its left edge — **USB-C port is at the top**.
  - **Right Edge Rest (Portrait CW)**: Device resting on its right edge — **USB-C port is at the bottom**.
* **Hysteresis & Debounce Filtering**: Uses a $350\text{ ms}$ time-window hysteresis filter on vertical acceleration thresholds ($A_y > +0.50g$ for normal, $A_y < -0.50g$ for inverted) to eliminate accidental flipping from minor vibrations.
* **Synchronized Display DMA & Capacitive Touch Mapping**: When inverted, both the 16-bit DMA scan buffer and the touch input driver synchronously invert coordinates ($X \leftrightarrow 640 - X, Y \leftrightarrow 172 - Y$), ensuring buttons and gestures remain natural.

---

## 🛠️ 3-Panel Spirit Level & Guided Inclinometer

<p align="center">
  <img src="../document/screen_spirit_level_panel1.png" alt="Raw IMU & Named Orientations" width="48%" />
  <img src="../document/screen_spirit_level_panel2.png" alt="2D Spirit Level & Dual Inclinometers" width="48%" />
</p>
<p align="center">
  <img src="../document/screen_spirit_level_panel3.png" alt="Guided 8-Step Calibration Wizard" width="60%" />
</p>

### Panel 1 (`[01] RAW DATA`):
- Live calibrated 6-axis accelerometer telemetry ($m/s^2$) with real-time sparkline waveform charts.
- 4-row named posture tracker (`[01] NORMAL LANDSCAPE`, `[02] TILTED LANDSCAPE`, `[03] PORTRAIT`, `[04] FLAT`) with 3-state badges (`[ACTIVE]` green, `[INVERTED]` amber, `[IDLE]` slate).
- Interactive `[-]` / `[+]` 0.25° tolerance adjustment steppers and single-tap `[↺]` reset button restoring default $\pm 2.00^\circ$ level lock.

### Panel 2 (`[02] SPIRIT LEVEL`):
- High-precision 2D bullseye reticle with liquid bubble physics (air bubble rises uphill to highest elevation edge across all orientations).
- High-precision `SHELF >` horizontal carpenter level ($\pm 10.0^\circ$ full scale, $0.1^\circ$ resolution, turning vibrant green when within tolerance).
- `PLUMB >` depth wall inclinometer ($\pm 15.0^\circ$), real-time degree readouts, and `[LOCKED] LEVEL` status indicator.

### Panel 3 (`[03] CALIBRATION`):
- 8-step guided calibration wizard with 0.5s hand stabilization delay.
- Dynamic isometric device wireframe diagram updating chassis posture, button orientation (`BTNS: UP`/`BTNS: DN`), and USB port (`USB: RIGHT`/`USB: LEFT`/`USB: TOP`/`USB: BOT`) per step across all 8 poses:
  1. *Wall Upright* (buttons up, USB right)
  2. *Wall Tilted Up* (stand angle)
  3. *Inverted Upright* (buttons down, USB left)
  4. *Inverted Tilted Down*
  5. *Left Edge Rest* (USB port on top)
  6. *Right Edge Rest* (USB port at bottom)
  7. *Desk Face-Up* (flat level)
  8. *Desk Face-Down*
- Calibration bias offsets are saved to NVS and directly applied across the system and display auto-rotation!
