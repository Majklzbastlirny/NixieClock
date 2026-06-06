# 3D-printed enclosure

Printable parts for the clock case. All files are `.3mf`. Print rigid parts in
PLA/PETG; print the feet in **TPU**.

| Part | File | Qty | Supports | Notes |
|------|------|-----|----------|-------|
| Main body / shell | [`main-body.3mf`](main-body.3mf) | 1 | **Yes** | The enclosure everything mounts inside. Takes 2× M3 heat-set inserts (see *Hardware*). |
| Rear panel | [`rear-panel.3mf`](rear-panel.3mf) | 1 | No | MMU / multi-color: printed labels for each rear button, the temperature-probe jacks, and the barrel-jack voltage. Screws onto the main body. |
| Component panel | [`component-panel.3mf`](component-panel.3mf) | 1 | **Yes** | Internal bracket that holds the accessory hardware: alarm speaker, RTC coin-cell holder, and the snooze buttons. |
| Snooze button cap | [`snooze-button-cap.3mf`](snooze-button-cap.3mf) | 2 | No | The physical snooze actuator (2 pieces). Sits in the component panel. |
| Front bezel | [`front-bezel.3mf`](front-bezel.3mf) | 1 | No | Hides the rough-cut front plexiglass edge. **Scale to 98 %** to fit as-is — or shave the lip if you print at 100 %. |
| Foot | [`foot-tpu.3mf`](foot-tpu.3mf) | 2 | No | Print in **TPU** (flexible). |

## Hardware (case assembly)

- **2× M3 heat-set threaded inserts** — installed in the main body.
- **2× M3×4 screws** — fasten the rear panel into those inserts.

(Electronics/wiring hardware is covered in the main project docs.)

## Print tips

- The main body, component panel use supports; orient so the support contact
  stays off visible faces.
- The rear panel is set up for a multi-material (MMU) printer so the labels come
  out in a contrasting color; on a single-material printer it still prints fine,
  the markings just won't be color-separated.
- Front bezel: if it's tight, the 98 % scale is the easy fix; the alternative is
  trimming the locating lip.
