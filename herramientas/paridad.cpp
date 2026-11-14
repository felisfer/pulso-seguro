// paridad.cpp · Corre features_fc.h (el mismo código del ESP32) sobre un dataset.csv
// y escribe las features a stdout en CSV. Lo usa probar_paridad.py.
//
//   g++ -O2 -std=c++17 -I firmware/pulso_seguro_aws herramientas/paridad.cpp -o paridad
//   ./paridad dataset.csv > features_c.csv
//
// Espera las columnas: sesion,label,sujeto,seq,t_ms,red,ir,ax,ay,az,gx,gy,gz
// (orden libre: se buscan por nombre en el encabezado).
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "features_fc.h"

static fcfeat::Extractor ext;   // estático: ~32 KB, como en el ESP32

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "uso: paridad dataset.csv\n"); return 1; }
  std::ifstream in(argv[1]);
  std::string linea;
  auto quitarCR = [](std::string& l) { if (!l.empty() && l.back() == '\r') l.pop_back(); };  // CSV de Windows
  std::getline(in, linea); quitarCR(linea);
  std::map<std::string, int> col;
  { std::stringstream ss(linea); std::string c; int i = 0; while (std::getline(ss, c, ',')) col[c] = i++; }
  const char* req[] = {"sesion", "seq", "red", "ir", "ax", "ay", "az", "gx", "gy", "gz"};
  for (auto r : req) if (!col.count(r)) { fprintf(stderr, "falta columna %s\n", r); return 1; }

  printf("sesion,win,valida");
  const char* nombres[] = {"fc", "fc_fft", "cv_ibi", "perfusion", "razon_r", "acc_media", "acc_std", "acc_frec",
                           "gyro_std", "fc_lag1", "fc_lag2", "fc_lag3", "acc_std_lag1", "acc_std_lag2",
                           "acc_std_lag3", "tendencia"};
  for (auto n : nombres) printf(",%s", n);
  printf("\n");

  std::string sesionActual;
  std::vector<std::string> v;
  while (std::getline(in, linea)) {
    quitarCR(linea);
    if (linea.empty()) continue;
    v.clear();
    std::stringstream ss(linea); std::string c;
    while (std::getline(ss, c, ',')) v.push_back(c);
    const std::string& ses = v[col["sesion"]];
    if (ses != sesionActual) { ext = fcfeat::Extractor(); sesionActual = ses; }  // como reiniciar el ESP32
    auto F = [&](const char* k) { return std::stof(v[col[k]]); };
    if (ext.push((uint32_t)std::stoul(v[col["seq"]]), F("red"), F("ir"), F("ax"), F("ay"), F("az"),
                 F("gx"), F("gy"), F("gz"))) {
      fcfeat::Fila f;
      ext.snapshot();
      ext.calcular(f);
      printf("%s,%u,%d", ses.c_str(), f.win, f.valida ? 1 : 0);
      for (int i = 0; i < fcfeat::N_FEATURES; i++) printf(",%.9g", (double)f.f[i]);
      printf("\n");
    }
  }
  return 0;
}
