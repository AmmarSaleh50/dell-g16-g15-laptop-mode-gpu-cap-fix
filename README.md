# Dell "Laptop Mode" GPU power cap — fix for the G15 5530 / G16 7630 (25 W / 30 W throttle)

Your RTX 4070/4060 Laptop GPU clamps itself to **25 W or 30 W** out of its ~140 W budget,
mid-game, and stays there. Frame rate collapses, latency doubles, and nothing you try in the
BIOS, Windows or the NVIDIA panel brings it back. This repo explains exactly what the cap is,
why every published fix fails on these machines, and ships a small tool that undoes it in about
two seconds — using NVIDIA's own driver library, nothing patched or injected.

### What is "Laptop Mode"?

**"Laptop Mode" is Dell's own name for this cap** (it's the title of KB article 000215120), and it's
what most owners know the problem by. It's a power state the laptop's embedded controller switches
on — supposedly when it senses the machine being tilted, shaken or moved — that drops the GPU to a
fraction of its power budget and *latches there*, often until a reboot, sometimes not even then. It
is not a thermal event: it fires with the GPU cold and the fans already at full. There is no setting
anywhere in Windows, the BIOS or the NVIDIA panel to turn it off, which is why it's so maddening.
This tool turns it back off.

Affected machines (Dell's own KB articles **000215120** and **000217923**): **Dell G15 5530**
and **Dell G16 7630**. Same embedded-controller behaviour, same symptom.

> **Disclaimer.** This talks only to the NVIDIA display driver through its own signed library.
> It installs no driver, patches no firmware, modifies no files and needs no admin rights — see
> §7. It is provided as-is with no warranty; you run it at your own risk. If you game with an
> anti-cheat, note that running *any* third-party tool alongside one is never a guaranteed-zero
> risk, however benign the tool.

---

## 1. The fault

Frame rate collapses (e.g. ~250–300 fps to ~160), render latency roughly doubles, the whole
machine feels like a slideshow. `nvidia-smi` shows the real story:

```
$ nvidia-smi --query-gpu=pstate,power.draw,enforced.power.limit,clocks_event_reasons.active --format=csv
P4, 25.06 W, 25.00 W, 0x0000000000000004
```

`enforced.power.limit` drops from 140 W to 25 W (sometimes 30 W). Two variants were observed:

| Limit | Reason bits | When |
|------:|-------------|------|
| 30 W | `0x4` (SW power cap) | mid-game, mid-session |
| 25 W | `0x24` (SW power cap + SW thermal) | from boot, and after a forced shutdown |

It is **not** heat. When it struck at 11:38 the CPU was 65 °C, the GPU 44 °C and the fans were at
maximum. Dell sets a "thermal" flag that has little to do with actual temperature.

### Dell's position

Dell documents this as expected behaviour on exactly these two models:

- **KB 000215120** — *Triggering Laptop Mode on Dell G15 5530 and G16 7630 Laptops Limits the Power
  to the GPU.* Says holding the laptop at an angle, or "excessive shaking, movement of the laptop",
  switches on a mode that caps GPU power. Remedy offered: keep it flat, don't move it, restart.
- **KB 000217923** — *GPU Power Limited to 30 W Under Extreme Conditions on Dell G15 5530.*

Owners have been asking for corrective firmware for over a year and none has shipped. BIOS 1.35.0 —
newer than anything on Dell's public catalogue — still has it.

### The most common fix — "disable the Intel sensor" — does not work

The workaround shared all over Reddit and Dell's forums is to disable the **Intel Integrated Sensor
Solution** (the motion/orientation sensor, sometimes called ISH) in Device Manager. **It does not
stop Laptop Mode.** It was tested directly here — sensor disabled, cap still appeared — and multiple
owners of both the G15 5530 and G16 7630 report the same failure.

The reason is that the trigger is **not** the Intel sensor the OS exposes. The decision to clamp,
and the clamp command itself, come from the laptop's **embedded controller (EC)**: the EC raises an
ACPI D-notifier (`0xD1`..`0xD5`) on the GPU, which the NVIDIA driver turns into the power-state
change traced in §3. That logic lives in firmware, *below* Windows — so disabling the Intel sensor
device leaves the EC's own trigger path completely untouched and nothing changes. This is also why
there is no setting anywhere in Windows, the BIOS or the NVIDIA panel that turns it off, and why the
only thing that reliably brings the GPU back is sending it to its full power state directly — which
is what this tool does.

**The published state of the art does not work on these machines.** The most careful public
investigation (GitHub issue #1 on `CyberMXSP/DELL-G15---Throttled-Profile-Manager-Linux-`) reaches
the same conclusion about the trigger (the Intel ISH motion sensor) and fixes it by blacklisting the
ISH kernel modules. Disabling ISH was tried here and the cap still appeared. That investigation then
stops exactly where every other one does — "the limit is managed above the NVIDIA driver, in
firmware or VBIOS" — which is the wrong turn described in §4: nobody looked at what *receives* the
signal.

---

## 2. Everything that did not work

Recorded so nobody repeats it.

| Attempt | Result |
|---|---|
| Restart | Cap often returns within minutes; once survived 21 min of idle |
| Full shutdown overnight | Still capped on next boot |
| Flea-power drain (hold power 15–20 s, charger out) | No change |
| Charger replug | No change; on battery it once ran at 60 W |
| Laptop flat on a hard surface, untouched | Still capped |
| Disabling Intel Integrated Sensor Solution (ISH) | No change — the detection is not in Windows |
| Stopping Intel IPF (`ipfsvc`, the DTT policy engine) | Clears the thermal flag bit, keeps the 25 W limit |
| Restarting NVIDIA Platform Controllers (`ACPI\NVDA0820\NPCF`) | No change |
| CPU clock cap | No change |
| `nvidia-smi -pl` | Not supported on laptop GPUs |
| MSI Afterburner power slider | Laptop GPUs do not expose it |
| BIOS 1.34.0 → 1.35.0 (EC 1.17.0) | Helped for hours, then the cap returned |
| BIOS downgrade | Blocked by BIOS Guard |
| Dell BIOS WMI (`root\dcim\sysman\biosattributes`) | No lap/motion setting exists — 2196 options checked |
| Registry search of `nvlddmkm.sys` (114 MB) | No switch to ignore D-notifiers; only `RmDisableACPI`, far too broad |

The one thing that *did* help was **G-mode** (Fn+F9): turning it on released the cap after ~3½
minutes. Not a fix — just Dell's policy engine changing its mind — and it stopped working reliably
later the same day.

### Two approaches deliberately refused

- **Patching the firmware.** The decision logic lives in the embedded controller. Its image is a
  Nuvoton NPCX blob, fully encrypted (entropy 7.99, 99.6 % of bytes differ between versions), and
  Dell-signed; the BIOS itself is locked by Intel Boot Guard. Weeks of work, real brick risk.
- **A kernel debugger.** Attaching one needs `bcdedit /debug on`, which some anti-cheats read
  directly and refuse to launch against.

---

## 3. How the cap actually works

Traced through NVIDIA's open-source driver (`NVIDIA/open-gpu-kernel-modules` @ `ca2d03e`), which
shares its RM core with the Windows driver:

```
Dell EC decides to clamp
   └─> ACPI Notify 0xD1 .. 0xD5 on the GPU device
         └─> rm_acpi_notify()                      osapi.c
               └─> RmHandleDNotifierEvent()        osapi.c:1370
                     └─> RM control 0x20802092     NV2080_CTRL_CMD_PERF_SET_AUX_POWER_STATE
                           powerState = 0..4
```

```c
// ctrl2080perf.h
#define NV2080_CTRL_CMD_PERF_SET_AUX_POWER_STATE (0x20802092)
typedef struct { NvU32 powerState; } NV2080_CTRL_PERF_SET_AUX_POWER_STATE_PARAMS;
#define NV2080_CTRL_PERF_AUX_POWER_STATE_P0 (0x00000000)   // full power
#define NV2080_CTRL_PERF_AUX_POWER_STATE_P4 (0x00000004)   // the 25 W clamp
```

`D1 → P0` (full power), `D5 → P4` (25 W). **The entire cap is one RM control with one 32-bit
argument.** The Windows driver uses the same machinery — its perf-limit table contains the string
`PERF_LIMIT_AUX_POWER`, which is precisely this limiter.

Critically, that control is flagged **non-privileged**. In the driver's export table
(`g_subdevice_nvoc.c` entry 484) it carries `flags = 0x8, accessRight = 0` — the same `0x8` as
`SetIllum` (RGB lighting) and `GpuGetInfoV2` (which `nvidia-smi` calls constantly), while
kernel-only internals carry `0x40 / 0x80 / 0xc0`. Independently, the `nestrilabs/virtio-nvgpu`
project lists `0x20802092` on its allow-list for *unprivileged* guest clients, with payload size 4.

So an ordinary user-mode program is permitted to send it.

---

## 4. Finding the way in on Windows

On Linux this is a plain `ioctl` on `/dev/nvidiactl` with fully public structures. Windows wraps
the same thing in a private envelope. Recovering it:

**The System32 DLLs are shims.** `nvapi64.dll` and `nvml.dll` import only `KERNEL32`, export two
functions, and have encrypted strings. The real implementations live in the driver store:

```
C:\Windows\System32\DriverStore\FileRepository\nvdmi.inf_amd64_<hash>\
    nvapi64_impl.dll   ~5.4 MB   imports D3DKMTEscape, keeps C++ RTTI names
    nvml.dll           ~3.6 MB
```

`nvapi64_impl.dll` still carries its escape structure type names — including
`NV_ESC_NVAPI_GPU_PERF_RMCTRLS`, a passthrough for PERF-category RM controls. Exactly our category.

Disassembled with `objdump -d` (free; no Ghidra or IDA needed):

**The envelope:**

```
+0x00  0x4E564441   magic, 'NVDA'
+0x04  0x00010002   version
+0x08  size         total buffer size
+0x0C  0x4E562A2A   second magic
+0x10  0x07000046   escape function: GPU PERF RM controls
+0x2C  status out
+0x30  GPU id
+0x34  RM control command
+0x38  control parameters
```

**nvapi sending the exact command:**

```asm
mov  %r14d,       0x30(%rbx)    ; GPU object handle
movl $0x20802092, 0x34(%rbx)    ; NV2080_CTRL_CMD_PERF_SET_AUX_POWER_STATE
movl $0x4,        0x38(%rbx)    ; power state
...
mov  $0x7000046,  %ecx          ; escape function code
call <dispatcher>
```

**Two transports**, both found: `D3DKMTEscape` (`Type = 0`, `Flags = 0x8`) and `DeviceIoControl`
on `\\.\NvAdminDevice` with code `0x08DE0008`, envelope at offset `0x0C`.

### Both hand-built transports were rejected

| Attempt | Result |
|---|---|
| `D3DKMTEscape`, `Type=0` | `0xC000000D` STATUS_INVALID_PARAMETER, every size, every flag |
| `D3DKMTEscape`, `Type=1` | `0xC0000022` ACCESS_DENIED — proving the struct itself was correct |
| Sweeping header word `+0x20` over 0..0xFFF | No change |
| `DeviceIoControl 0x08DE0008` | error 87 — device opens fine, unprivileged |
| `DeviceIoControl 0x08DE0010` | error 1168 NOT_FOUND — needs a session handle from nvapi's own init |

Replicating nvapi's full initialisation handshake would have been another long haul — so instead,
let nvapi do it.

---

## 5. The way in

`nvapi64.dll` exports `nvapi_QueryInterface(id)`. The id → function table sits in
`nvapi64_impl.dll`'s `.data`: `{ u64 function pointer, u32 id, u32 padding }` entries, verifiable
against published NvAPI ids:

```
id 0x0150E828 -> NvAPI_Initialize
id 0xE5AC921F -> NvAPI_EnumPhysicalGPUs
id 0xCEEE8E9F -> NvAPI_GPU_GetFullName
id 0x48E0847D -> the aux-power setter
```

**`nvapi_QueryInterface(0x48E0847D)`** returns the routine that issues the control, and NVIDIA's own
library performs all the plumbing that could not be replicated by hand. Signature:

```c
int (*)(NvPhysicalGpuHandle hGpu, NvU32 index);
```

### Why an out-of-range index restores full power

Look at how that routine turns `index` into a power state:

```asm
test %esi,%esi
je   .write1            ; index 0 -> powerState 1
sub  $0x1,%esi
je   .write2            ; index 1 -> powerState 2
sub  $0x1,%esi
je   .write3            ; index 2 -> powerState 3
cmp  $0x1,%esi
jne  .send              ; anything else -> writes NOTHING, buffer stays zeroed
movl $0x4,0x38(%rbx)    ; index 3 -> powerState 4
```

The buffer is `memset` to zero beforehand. So NVIDIA's library can step the GPU **down** through
P1–P4, but has no branch that writes `0` — it has no explicit "restore full power" path. Yet an
**out-of-range index falls through writing nothing at all**, leaving `powerState = 0 = P0 = full
power`.

`gpu-uncap` passes index **99**. Nothing is corrupted, no memory is overwritten, no privilege is
gained — it is NVIDIA's own routine, called through its own public entry point, with an input its
authors simply didn't add a branch for.

### Proof

```
$ ./gpu-uncap.exe -n 3     # force the clamp
  NVIDIA GeForce RTX 4070 Laptop GPU: accepted (0)
  1 GPU(s) accepted; limit 140 W -> 10 W

$ ./gpu-uncap.exe          # restore
  NVIDIA GeForce RTX 4070 Laptop GPU: accepted (0)
  1 GPU(s) accepted; limit 10 W -> 140 W
```

Instant, repeatable, no restart.

---

## 6. Build and run

No dependencies beyond a C compiler. With [MinGW-w64](https://www.mingw-w64.org/) `gcc` on the PATH:

```
gcc -O2 -o gpu-uncap.exe gpu-uncap.c
```

| Command | Effect |
|---|---|
| `gpu-uncap.exe` | restore full power once |
| `gpu-uncap.exe -w` | watch: check twice a second, restore whenever the limit drops below 100 W |
| `gpu-uncap.exe -r` | read the limit only, change nothing |
| `gpu-uncap.exe -n IDX` | send index `IDX` (0–3 step down, anything else = full power) |
| `gpu-uncap.exe -i SEC` | poll interval, seconds (default 0.5) |
| `gpu-uncap.exe -m WATTS` | the threshold that counts as capped (default 100) |

Most people want **watch mode**: `gpu-uncap.exe -w`. `watch.ps1` starts it hidden and single-instance,
and you can drop a shortcut to it in `shell:startup` to run it at login. Watch-mode log:
`%APPDATA%\gpu-uncap\gpu-uncap.log`.

### How fast, and can it flap?

The watcher reads the limit through **NVML in-process** — the library `nvidia-smi` itself uses — so
each check costs microseconds and spawns nothing. A 0.5 s poll is effectively free, and a clamp is
undone within roughly **half a second** of Dell applying it.

Dell's controller sends its clamp **once, when it changes its mind** — not continuously. In practice
the cap is applied once and the restore holds for the rest of the session. If it ever *did* re-clamp
repeatedly, the watcher spaces restores at least **1 s apart** (it can never hammer the driver) and
logs each one with a 10-minute count, so flapping would show up immediately as a climbing number.
Each restore uses the driver's normal, designed path — the same message Dell's own D1 notify
produces.

---

## 7. Why this is safe to run

- **No driver is loaded.** Nothing is installed, nothing signed or unsigned enters the kernel.
- **No signature check is bypassed.** Secure Boot and driver signing are untouched.
- **Nothing is patched.** No firmware, no BIOS, no driver binary is modified. The DLLs were only
  ever *read*.
- **No admin rights.** It runs as a normal user.
- **It talks only to the display driver,** through NVIDIA's own signed library — no injection into
  any other process, no network traffic.

It is strictly less invasive than tools like MSI Afterburner, which inject an overlay. With any
third-party tool the risk is low but, honestly, never exactly zero.

---

## Appendix — key constants

```
RM control            0x20802092   NV2080_CTRL_CMD_PERF_SET_AUX_POWER_STATE
  payload                       4   bytes, one NvU32
  P0 full power                 0
  P4 clamp (25 W)               4
ACPI notify           0xD1..0xD5   D1 -> P0, D5 -> P4
NvAPI id              0x48E0847D   aux-power setter
NvAPI id              0x0150E828   NvAPI_Initialize
NvAPI id              0xE5AC921F   NvAPI_EnumPhysicalGPUs
escape function       0x07000046   GPU PERF RM controls
envelope magic        0x4E564441   'NVDA'
envelope magic 2      0x4E562A2A
envelope version      0x00010002
ioctl                 0x08DE0008   \\.\NvAdminDevice
```

---

## License

MIT — see [LICENSE](LICENSE).
