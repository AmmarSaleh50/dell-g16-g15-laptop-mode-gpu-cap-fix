// Undo Dell's GPU power cap (the "laptop mode" 25 W / 30 W clamp) from user mode.
//
// What the cap is: Dell's embedded controller raises an ACPI notify 0xD1..0xD5 on the GPU; the NVIDIA driver turns
// that into one RM control, NV2080_CTRL_CMD_PERF_SET_AUX_POWER_STATE (0x20802092), whose single argument is the
// auxiliary power state: 0 = P0 = full power, 4 = P4 = the 25 W clamp.
//
// How this sends it: through NVIDIA's own nvapi64.dll, the library the NVIDIA control panel and every GPU tool
// use. Its dispatch table (recovered from nvapi64_impl.dll in the driver store) maps id 0x48E0847D to the
// internal routine that issues exactly that RM control. No driver is loaded, no signature check is bypassed,
// nothing is patched, nothing is injected into another process, and the game client is never touched.
//
// The quirk this relies on: that routine writes 1, 2, 3 or 4 into the power-state field for index 0, 1, 2 or 3,
// and for any other index writes nothing at all - leaving the zeroed buffer's 0 = P0 = full power. NVIDIA's own
// library can step the GPU down but never back up; an out-of-range index is what sends it back up.
//
//   build:  gcc -O2 -o gpu-uncap.exe gpu-uncap.c
//   run:    gpu-uncap.exe            restore full power once
//           gpu-uncap.exe -w         watch: restore whenever the limit drops (polls NVML every 0.5 s)
//           gpu-uncap.exe -r         read the limit only, change nothing
//           gpu-uncap.exe -n IDX     send index IDX instead (0..3 step down, anything else = full power)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ID_INITIALIZE    0x0150E828u
#define ID_ENUM_GPUS     0xE5AC921Fu
#define ID_GET_FULL_NAME 0xCEEE8E9Fu
#define ID_SET_AUX_POWER 0x48E0847Du   // internal: issues NV2080_CTRL_CMD_PERF_SET_AUX_POWER_STATE
#define FULL_POWER_IDX   99u           // out of range on purpose -> power state stays 0 (P0)
#define MAX_GPUS         64

typedef void *NvGpu;
typedef void *(__cdecl *PFN_QI)(unsigned int);
typedef int(__cdecl *PFN_INIT)(void);
typedef int(__cdecl *PFN_ENUM)(NvGpu *, unsigned int *);
typedef int(__cdecl *PFN_NAME)(NvGpu, char *);
typedef int(__cdecl *PFN_AUX)(NvGpu, unsigned int);

static PFN_ENUM pEnum;
static PFN_NAME pName;
static PFN_AUX  pAux;

static int nvapi_load(void) {
  HMODULE m = LoadLibraryW(L"nvapi64.dll");
  if (!m) return 0;
  PFN_QI qi = (PFN_QI)GetProcAddress(m, "nvapi_QueryInterface");
  if (!qi) return 0;
  PFN_INIT init = (PFN_INIT)qi(ID_INITIALIZE);
  pEnum = (PFN_ENUM)qi(ID_ENUM_GPUS);
  pName = (PFN_NAME)qi(ID_GET_FULL_NAME);
  pAux = (PFN_AUX)qi(ID_SET_AUX_POWER);
  if (!init || !pEnum || !pAux) return 0;
  return init() == 0;
}

// Reading the limit: NVML in-process (microseconds, no child process - cheap enough to poll twice a second during a
// game), falling back to spawning nvidia-smi when NVML is unavailable.
typedef int(__cdecl *PFN_NVML_INIT)(void);
typedef int(__cdecl *PFN_NVML_HANDLE)(unsigned int, void **);
typedef int(__cdecl *PFN_NVML_LIMIT)(void *, unsigned int *);
static PFN_NVML_LIMIT pNvmlLimit;
static void *nvmlDev;

static int nvml_load(void) {
  HMODULE m = LoadLibraryW(L"nvml.dll");
  if (!m) return 0;
  PFN_NVML_INIT init = (PFN_NVML_INIT)GetProcAddress(m, "nvmlInit_v2");
  PFN_NVML_HANDLE handle = (PFN_NVML_HANDLE)GetProcAddress(m, "nvmlDeviceGetHandleByIndex_v2");
  pNvmlLimit = (PFN_NVML_LIMIT)GetProcAddress(m, "nvmlDeviceGetEnforcedPowerLimit");
  if (!init || !handle || !pNvmlLimit) return 0;
  return init() == 0 && handle(0, &nvmlDev) == 0;
}

static double limit_smi(void);

// The GPU's enforced power limit in watts (0 when it cannot be read).
static double limit_w(void) {
  unsigned int mw = 0;
  if (pNvmlLimit && pNvmlLimit(nvmlDev, &mw) == 0) return mw / 1000.0;
  return limit_smi();
}

static double limit_smi(void) {
  FILE *f = _popen("nvidia-smi --query-gpu=enforced.power.limit --format=csv,noheader,nounits 2>nul", "r");
  if (!f) return 0;
  char line[64] = { 0 };
  double v = fgets(line, sizeof line, f) ? atof(line) : 0;
  _pclose(f);
  return v;
}

// Send the aux power index to every NVIDIA GPU; returns how many accepted it.
static int send_all(unsigned int idx, int verbose) {
  NvGpu gpu[MAX_GPUS];
  unsigned int n = 0;
  int st = pEnum(gpu, &n), ok = 0;
  if (st != 0) { if (verbose) printf("  EnumPhysicalGPUs failed (%d)\n", st); return 0; }
  for (unsigned int i = 0; i < n; i++) {
    char name[128] = { 0 };
    if (pName) pName(gpu[i], name);
    int r = pAux(gpu[i], idx);
    if (verbose) printf("  %s: %s (%d)\n", name[0] ? name : "GPU", r == 0 ? "accepted" : "rejected", r);
    if (r == 0) ok++;
  }
  return ok;
}

// "13:54:01" for the log
static const char *now_hms(void) {
  static char s[32];
  SYSTEMTIME t;
  GetLocalTime(&t);
  sprintf(s, "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
  return s;
}

int main(int argc, char **argv) {
  unsigned int idx = FULL_POWER_IDX;
  int watch = 0, interval_ms = 500, quiet = 0, readonly = 0;
  double floor_w = 100.0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-n") && i + 1 < argc) idx = (unsigned int)strtoul(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "-w")) watch = 1;
    else if (!strcmp(argv[i], "-i") && i + 1 < argc) interval_ms = (int)(atof(argv[++i]) * 1000);
    else if (!strcmp(argv[i], "-m") && i + 1 < argc) floor_w = atof(argv[++i]);
    else if (!strcmp(argv[i], "-q")) quiet = 1;
    else if (!strcmp(argv[i], "-r")) readonly = 1;
    else { printf("usage: %s [-n IDX] [-w] [-i SEC] [-m WATTS] [-q] [-r]\n", argv[0]); return 2; }
  }
  if (interval_ms < 100) interval_ms = 100;
  int nvml = nvml_load();

  if (readonly) {   // read the limit both ways and change nothing
    unsigned int mw = 0;
    int ok = pNvmlLimit && pNvmlLimit(nvmlDev, &mw) == 0;
    printf("nvml: %s%.0f W   nvidia-smi: %.0f W\n", ok ? "" : "unavailable ", mw / 1000.0, limit_smi());
    return ok ? 0 : 1;
  }
  if (watch) {   // one watcher at a time, whoever starts it
    HANDLE mx = CreateMutexW(NULL, TRUE, L"Global\\GpuUncapWatch");
    if (!mx || GetLastError() == ERROR_ALREADY_EXISTS) { printf("already watching\n"); return 0; }
  }
  if (!nvapi_load()) { printf("nvapi64.dll: could not initialise\n"); return 1; }

  if (!watch) {
    double before = limit_w(), after = before;
    int n = send_all(idx, !quiet);
    for (int t = 0; t < 20 && after <= before; t++) { Sleep(150); after = limit_w(); }
    if (!quiet) printf("%d GPU(s) accepted; limit %.0f W -> %.0f W\n", n, before, after);
    return (n && after >= before) ? 0 : 1;
  }

  // Dell's controller sends its clamp once, when it changes its mind - not continuously - so normally this restores
  // once and the limit stays put. If it ever did re-clamp over and over, restores are spaced at least MIN_GAP_MS
  // apart (never hammering the driver) and every one is logged with a count of how many happened in the last 10
  // minutes, so flapping would show up plainly in the log.
  #define MIN_GAP_MS 1000
  #define WINDOW_MS  (10 * 60 * 1000)
  #define MAX_HIST   256
  #define NOTE_GAP_MS 60000   // how often an ordinary limit change may be logged
  ULONGLONG hist[MAX_HIST];
  int nh = 0;
  ULONGLONG last = 0, last_note = 0;

  printf("%s watching every %d ms (%s): any limit below %.0f W is restored to full power\n", now_hms(), interval_ms,
         nvml ? "nvml" : "nvidia-smi", floor_w);
  fflush(stdout);
  double was = -1;
  for (;;) {
    double w = limit_w();
    ULONGLONG t = GetTickCount64();
    if (w > 0 && w < floor_w && t - last >= MIN_GAP_MS) {
      send_all(idx, 0);
      double after = w;
      for (int k = 0; k < 20 && after < floor_w; k++) { Sleep(50); after = limit_w(); }
      ULONGLONG done = GetTickCount64();
      last = done;
      // restores within the last 10 minutes
      int keep = 0;
      for (int k = 0; k < nh; k++) if (done - hist[k] < WINDOW_MS) hist[keep++] = hist[k];
      nh = keep;
      if (nh < MAX_HIST) hist[nh++] = done;
      printf("%s clamp %.0f W -> restored %.0f W in %llu ms   (%d in the last 10 min)\n", now_hms(), w, after,
             (unsigned long long)(done - t), nh);
      fflush(stdout);
      was = after;
    } else if (w != was) {
      // Dynamic Boost moves the ceiling between ~110 W and 140 W every half second, which is normal and not
      // worth a log line each time - that alone was ~5 MB a day. Only note a change that is big enough to
      // mean something, and at most once a minute, so a long session stays readable and small. Clamps above
      // are always logged, however often they happen.
      double moved = w > was ? w - was : was - w;
      if (moved >= 10.0 && t - last_note >= NOTE_GAP_MS) {
        printf("%s limit %.0f W\n", now_hms(), w);
        fflush(stdout);
        last_note = t;
      }
      was = w;
    }
    Sleep(interval_ms);
  }
}
