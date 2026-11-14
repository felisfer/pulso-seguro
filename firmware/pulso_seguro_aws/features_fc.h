// features_fc.h · Extractor de features de PulsoSeguro (port de modelo/features.py)
//
// Header-only, sin dependencias de Arduino: compila en el ESP32 y en la PC
// (herramientas/paridad.cpp lo usa para comprobar que da lo mismo que Python).
//
// Si cambias el algoritmo aquí, cámbialo también en features.py y corre
//   python herramientas/probar_paridad.py dataset.csv
//
// Uso típico (ver INTEGRACION_FIRMWARE.md):
//   núcleo 1 (cada muestra, 100 Hz):  if (ext.push(seq, red, ir, ax..gz)) notificar tarea
//   tarea de features (núcleo 0):     ext.snapshot() [en sección crítica]; ext.calcular(fila)
//
// Memoria: ~30.5 KB con float (default), todo dentro del objeto Extractor. En el
// ESP32-WROOM no cabe como variable global (la DRAM estática ya está casi llena):
// prediccion_fc.h lo reserva en el heap al arrancar.
// Medido en probar_paridad: float vs double difiere < 1e-5 lpm en fc y 0 ventanas
// cambian de válida/inválida, así que float (FPU por hardware del ESP32) es lo indicado.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI                      // MSVC no lo define sin _USE_MATH_DEFINES
#define M_PI 3.14159265358979323846
#endif

#ifndef FC_REAL
#define FC_REAL float    // el ESP32 tiene FPU de precisión simple; double sería por software
#endif

namespace fcfeat {

constexpr int   FS = 100, VENTANA = 800, PASO = 200, N_DFT = 1024, MARGEN = 50;
constexpr float UMBRAL_DEDO = 50000.0f;
constexpr FC_REAL CV_MAX = 0.15, TOL_FFT = 10.0;
constexpr int   K_MIN = 6, K_MAX = 40;          // ceil(0.5*1024/100), floor(4*1024/100)
constexpr int   N_FEATURES = 16, VERSION_FEATURES = 1;

static const FC_REAL SOS[2][6] = {
  {0.01043241337109341, 0.02086482674218683, 0.01043241337109341, 1.0, -1.717844586580564, 0.7634885625932664},
  {1.0, -2.0, 1.0, 1.0, -1.9585309426687274, 0.9597079331261205},
};

// Orden idéntico a features.NOMBRES
struct Fila {
  uint32_t win;
  bool     valida;
  FC_REAL  f[N_FEATURES];   // fc, fc_fft, cv_ibi, perfusion, razon_r, acc_media, acc_std, acc_frec,
                            // gyro_std, fc_lag1..3, acc_std_lag1..3, tendencia
};

class Extractor {
 public:
  // --- núcleo 1: barato, O(1). Devuelve true cuando hay una ventana nueva lista.
  bool push(uint32_t seq, float red, float ir, float ax, float ay, float az, float gx, float gy, float gz) {
    if (hayUltimo_ && seq != ultimoSeq_ + 1) reiniciar();
    hayUltimo_ = true; ultimoSeq_ = seq;
    rRed_[cab_] = red; rIr_[cab_] = ir;
    rAmag_[cab_] = sqrtf(ax * ax + ay * ay + az * az);
    rGmag_[cab_] = sqrtf(gx * gx + gy * gy + gz * gz);
    cab_ = (cab_ + 1) % VENTANA;
    if (llenas_ < VENTANA) llenas_++;
    nuevas_++;
    if (llenas_ == VENTANA && nuevas_ >= PASO) { nuevas_ = 0; return true; }
    return false;
  }

  void reiniciar() { cab_ = 0; llenas_ = 0; nuevas_ = 0; hayUltimo_ = false; epoca_ = epoca_ + 1; }

  // --- copia la ventana del anillo (llamar en sección crítica si push() corre en otra tarea)
  void snapshot() {
    for (int i = 0; i < VENTANA; i++) {
      int j = (cab_ + i) % VENTANA;           // de la más vieja a la más nueva
      ir_[i] = rIr_[j]; red_[i] = rRed_[j]; amag_[i] = rAmag_[j]; gmag_[i] = rGmag_[j];
    }
    epocaSnap_ = epoca_;
  }

  // --- tarea de features: lo pesado (~2 DFT + 8 pasadas de biquad)
  void calcular(Fila& out) {
    if (epocaSnap_ != epocaHist_) { nHist_ = 0; epocaHist_ = epocaSnap_; }

    FC_REAL dcIr = media(ir_), dcRed = media(red_);
    bool dedo = dcIr > UMBRAL_DEDO;
    for (int i = 0; i < VENTANA; i++) { ir_[i] -= dcIr; red_[i] -= dcRed; }
    pasabanda(ir_); pasabanda(red_);

    FC_REAL fcFft = 60.0 * frecDominante(ir_);
    FC_REAL fc = 0, cv = 0; int nIbi = 0;
    if (fcFft > 0) {
      int dmin = (int)lround(0.6 * FS * 60.0 / fcFft); if (dmin < 1) dmin = 1;
      int nP = detectarPicos(ir_, dmin, picos_);
      nIbi = nP - 1;
      if (nIbi >= 1) {
        FC_REAL* ibi = ibi_; FC_REAL suma = 0;
        for (int i = 0; i < nIbi; i++) { ibi[i] = (FC_REAL)(picos_[i + 1] - picos_[i]) / FS; suma += ibi[i]; }
        FC_REAL m = suma / nIbi, var = 0;
        for (int i = 0; i < nIbi; i++) var += (ibi[i] - m) * (ibi[i] - m);
        cv = sqrt(var / nIbi) / m;
        fc = 60.0 / mediana(ibi, nIbi);
      }
    }
    FC_REAL sIr = desv(ir_), sRed = desv(red_);
    FC_REAL perf = dcIr > 0 ? 100.0 * sIr / dcIr : 0;
    FC_REAL r = (dcRed > 0 && dcIr > 0 && sIr > 0) ? (sRed / dcRed) / (sIr / dcIr) : 0;

    FC_REAL accMedia = mediaF(amag_), accStd = desvF(amag_), gyroStd = desvF(gmag_);
    for (int i = 0; i < VENTANA; i++) ir_[i] = amag_[i];        // reutilizo el buffer para la DFT del acel.
    FC_REAL accFrec = frecDominante(ir_);

    bool valida = dedo && nIbi >= 3 && cv < CV_MAX && fabs(fc - fcFft) <= TOL_FFT;

    out.win = win_++;
    out.valida = valida;
    FC_REAL* f = out.f;
    f[0] = fc; f[1] = fcFft; f[2] = cv; f[3] = perf; f[4] = r;
    f[5] = accMedia; f[6] = accStd; f[7] = accFrec; f[8] = gyroStd;
    for (int k = 1; k <= 3; k++) {                             // hist_[0] = ventana anterior
      bool hay = nHist_ >= k;
      const Hist& h = hist_[k - 1];
      f[8 + k]  = (hay && h.valida) ? h.fc : fc;               // fc_lag1..3
      f[11 + k] = hay ? h.accStd : accStd;                     // acc_std_lag1..3
    }
    f[15] = fc - f[11];                                        // tendencia = fc - fc_lag3

    hist_[2] = hist_[1]; hist_[1] = hist_[0]; hist_[0] = {valida, fc, accStd};
    if (nHist_ < 3) nHist_++;
  }

 private:
  struct Hist { bool valida; FC_REAL fc; FC_REAL accStd; };

  // anillo (lo escribe push)
  float rRed_[VENTANA], rIr_[VENTANA], rAmag_[VENTANA], rGmag_[VENTANA];
  int cab_ = 0, llenas_ = 0, nuevas_ = 0;
  uint32_t ultimoSeq_ = 0; bool hayUltimo_ = false;
  volatile uint32_t epoca_ = 0;
  // trabajo (lo usa calcular)
  FC_REAL ir_[VENTANA], red_[VENTANA];
  float amag_[VENTANA], gmag_[VENTANA];
  int16_t picos_[VENTANA];                 // índices < 800: int16 ahorra 1.6 KB
  int16_t cand_[VENTANA];                  // candidatos a pico (antes static dentro de la función)
  FC_REAL ibi_[VENTANA / 2];               // intervalos entre latidos
  uint32_t epocaSnap_ = 0, epocaHist_ = 0, win_ = 0;
  Hist hist_[3]; int nHist_ = 0;

  static FC_REAL media(const FC_REAL* x) { FC_REAL s = 0; for (int i = 0; i < VENTANA; i++) s += x[i]; return s / VENTANA; }
  static FC_REAL mediaF(const float* x) { FC_REAL s = 0; for (int i = 0; i < VENTANA; i++) s += x[i]; return s / VENTANA; }
  static FC_REAL desv(const FC_REAL* x) {
    FC_REAL m = media(x), v = 0; for (int i = 0; i < VENTANA; i++) v += (x[i] - m) * (x[i] - m); return sqrt(v / VENTANA);
  }
  static FC_REAL desvF(const float* x) {
    FC_REAL m = mediaF(x), v = 0; for (int i = 0; i < VENTANA; i++) v += (x[i] - m) * (x[i] - m); return sqrt(v / VENTANA);
  }

  static void biquad(FC_REAL* x, const FC_REAL* s, bool reversa) {
    FC_REAL z1 = 0, z2 = 0;
    for (int n = 0; n < VENTANA; n++) {
      int i = reversa ? VENTANA - 1 - n : n;
      FC_REAL xi = x[i], yi = s[0] * xi + z1;
      z1 = s[1] * xi - s[4] * yi + z2;
      z2 = s[2] * xi - s[5] * yi;
      x[i] = yi;
    }
  }
  static void pasabanda(FC_REAL* x) {      // filtfilt con estado inicial 0
    biquad(x, SOS[0], false); biquad(x, SOS[1], false);
    biquad(x, SOS[0], true);  biquad(x, SOS[1], true);
  }

  // DFT directa en los bins de 0.5-4 Hz. Las twiddles se generan por recurrencia
  // (rotación compleja) para no llamar a sin/cos 800 veces por bin.
  static FC_REAL frecDominante(const FC_REAL* x) {
    FC_REAL m = media(x);
    const FC_REAL HC = cos(2.0 * M_PI / (VENTANA - 1)), HS = sin(2.0 * M_PI / (VENTANA - 1));
    FC_REAL mags[K_MAX - K_MIN + 1];
    for (int k = K_MIN; k <= K_MAX; k++) {
      FC_REAL dth = 2.0 * M_PI * k / N_DFT, cd = cos(dth), sd = sin(dth);
      FC_REAL c = 1, s = 0, re = 0, im = 0;
      FC_REAL hc = 1, hs = 0;                     // cos/sen de la ventana de Hann, también por recurrencia
      for (int n = 0; n < VENTANA; n++) {
        FC_REAL xv = (x[n] - m) * (0.5 - 0.5 * hc);
        re += xv * c; im -= xv * s;
        FC_REAL c2 = c * cd - s * sd; s = s * cd + c * sd; c = c2;
        FC_REAL h2 = hc * HC - hs * HS; hs = hs * HC + hc * HS; hc = h2;
      }
      mags[k - K_MIN] = re * re + im * im;
    }
    int nb = K_MAX - K_MIN + 1, i = 0;
    for (int j = 1; j < nb; j++) if (mags[j] > mags[i]) i = j;
    if (mags[i] <= 0) return 0;
    FC_REAL delta = 0;
    if (i > 0 && i < nb - 1) {
      FC_REAL a = mags[i - 1], b = mags[i], c = mags[i + 1], den = a - 2 * b + c;
      if (den != 0) delta = 0.5 * (a - c) / den;
    }
    return (K_MIN + i + delta) * FS / N_DFT;
  }

  int detectarPicos(const FC_REAL* y, int dmin, int16_t* out) {
    int16_t* cand = cand_; int nc = 0;
    for (int i = MARGEN; i < VENTANA - MARGEN; i++)
      if (y[i] > 0 && y[i] > y[i - 1] && y[i] >= y[i + 1]) cand[nc++] = (int16_t)i;
    // ordenar por altura descendente (empate: índice menor). Inserción: nc es pequeño (< 100)
    for (int a = 1; a < nc; a++) {
      int v = cand[a], b = a - 1;
      while (b >= 0 && (y[cand[b]] < y[v] || (y[cand[b]] == y[v] && cand[b] > v))) { cand[b + 1] = cand[b]; b--; }
      cand[b + 1] = v;
    }
    int n = 0;
    for (int a = 0; a < nc; a++) {
      bool ok = true;
      for (int b = 0; b < n; b++) if (abs(cand[a] - out[b]) < dmin) { ok = false; break; }
      if (ok) out[n++] = cand[a];
    }
    for (int a = 1; a < n; a++) {             // ordenar por posición
      int v = out[a], b = a - 1;
      while (b >= 0 && out[b] > v) { out[b + 1] = out[b]; b--; }
      out[b + 1] = v;
    }
    return n;
  }

  static FC_REAL mediana(FC_REAL* v, int n) {   // ordena in situ (n pequeño)
    for (int a = 1; a < n; a++) { FC_REAL t = v[a]; int b = a - 1; while (b >= 0 && v[b] > t) { v[b + 1] = v[b]; b--; } v[b + 1] = t; }
    return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
  }
};

}  // namespace fcfeat
