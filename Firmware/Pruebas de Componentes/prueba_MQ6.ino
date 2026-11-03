/*
 * ============================================================
 *  PRUEBA INDIVIDUAL DEL SENSOR MQ-6 (Gas LP: propano / butano)
 *  Robot Centinela V7.2 - Banco de pruebas (circuito independiente)
 * ============================================================
 *
 *  El MQ-6 NECESITA 5V (su calentador interno no funciona con 3.3V),
 *  así que todo va del lado de VIN (parte de abajo a la izquierda):
 *
 *     MQ-6 (módulo)    ESP32 (NodeMCU-32S)
 *     -------------    -------------------
 *     VCC   ------->   VIN (5V del USB)
 *     GND   ------->   GND (pin entre D12 y D13)
 *     A0    --[10kΩ]--+-->  D32 (GPIO 32)
 *                     |
 *                   [20kΩ]
 *                     |
 *                    GND
 *     D0    ------->   sin conectar
 *
 *  El divisor 10k/20k es OBLIGATORIO: A0 puede subir hasta casi 5V
 *  con mucho gas y el ESP32 solo aguanta 3.3V. Si no tienes de 20kΩ,
 *  usa dos de 10kΩ en serie.
 *
 *  D13 funciona como entrada analógica porque en esta prueba NO se usa
 *  Wi-Fi (los pines analógicos de este lado no leen con Wi-Fi activo).
 *
 *  IMPORTANTE:
 *   - El sensor se CALIENTA, es normal. No lo toques por arriba.
 *   - Un MQ nuevo necesita varias horas encendido para estabilizarse;
 *     para esta prueba basta con el precalentamiento.
 *   - Las ppm son una ESTIMACIÓN (curva del datasheet).
 *
 *  CÓMO PROBARLO:
 *   Acerca un encendedor SIN PRENDERLO y suelta gas 1-2 s cerca del
 *   sensor. El gas del encendedor (butano) es justo lo que detecta el
 *   MQ-6, así que debe reaccionar fuerte. Nunca uses flama cerca.
 *
 *  Monitor Serie a 115200 baudios.
 *  LED azul integrado (GPIO 2): se enciende al detectar gas.
 * ============================================================
 */

// ---------------- CONFIGURACIÓN ----------------
const int PIN_A0  = 32;     // D32 (a través del divisor)
const int PIN_LED = 2;      // LED integrado

const float FACTOR_DIVISOR = 1.5;    // (10k + 20k) / 20k
const float VCC_SENSOR     = 5.0;
const float RL_KOHM        = 10.0;   // Resistencia de carga del módulo:
                                     //   "103" en la placa = 10k, "102" = 1k
const float RS_R0_AIRE     = 10.0;   // Relación Rs/R0 en aire limpio (datasheet MQ-6)

// Curva LPG del datasheet: ppm = A * (Rs/R0)^B
const float CURVA_A = 1009.2;
const float CURVA_B = -2.35;

const int   PRECALENTAMIENTO_S = 60;
const int   MUESTRAS_CALIB     = 50;
const float UMBRAL_ALERTA      = 0.70;   // Alerta si Rs cae al 70% de su valor en aire limpio
const unsigned long PERIODO_MS = 1000;
// ------------------------------------------------

float R0 = 0;
float rsAireLimpio = 0;

// Voltaje REAL en A0 (compensando el divisor), promediando lecturas
float leerVoltajeA0() {
  long suma = 0;
  const int n = 20;
  for (int i = 0; i < n; i++) {
    suma += analogReadMilliVolts(PIN_A0);
    delay(2);
  }
  float vPin = (suma / (float)n) / 1000.0;
  return vPin * FACTOR_DIVISOR;
}

// Resistencia del sensor (kΩ)
float calcularRs(float vA0) {
  if (vA0 < 0.01) vA0 = 0.01;
  return RL_KOHM * (VCC_SENSOR - vA0) / vA0;
}

void imprimirBarra(float valor, float maximo) {
  int bloques = constrain((int)(valor / maximo * 40.0), 0, 40);
  Serial.print(" |");
  for (int i = 0; i < bloques; i++) Serial.print("#");
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(PIN_LED, OUTPUT);

  Serial.println();
  Serial.println("==============================================");
  Serial.println("      PRUEBA DE SENSOR MQ-6 (GAS LP / BUTANO)");
  Serial.println("==============================================");
  Serial.println("VCC -> VIN | GND -> GND | A0 -> divisor -> D13");
  Serial.println();

  // ---- 1. Precalentamiento ----
  Serial.printf("1) Precalentando %d s (deja el sensor en aire limpio)...\n", PRECALENTAMIENTO_S);
  for (int s = PRECALENTAMIENTO_S; s > 0; s--) {
    if (s % 10 == 0 || s <= 5) {
      Serial.printf("   %3d s restantes | A0 = %.2f V\n", s, leerVoltajeA0());
    }
    digitalWrite(PIN_LED, s % 2);
    delay(1000);
  }
  digitalWrite(PIN_LED, LOW);

  // ---- 2. Calibración en aire limpio ----
  Serial.println("2) Calibrando en aire limpio...");
  float sumaRs = 0;
  for (int i = 0; i < MUESTRAS_CALIB; i++) {
    sumaRs += calcularRs(leerVoltajeA0());
    delay(50);
  }
  rsAireLimpio = sumaRs / MUESTRAS_CALIB;
  R0 = rsAireLimpio / RS_R0_AIRE;

  float vAire = VCC_SENSOR * RL_KOHM / (rsAireLimpio + RL_KOHM);
  Serial.printf("   Rs en aire limpio: %.2f kOhm | R0: %.2f kOhm | A0: %.2f V\n",
                rsAireLimpio, R0, vAire);

  if (vAire < 0.05) {
    Serial.println("   !! A0 casi en 0V: revisa A0 -> 10k -> D13, VCC en VIN y GND.");
  } else if (vAire > 3.5) {
    Serial.println("   !! A0 muy alto en aire limpio: sigue calentando o hay gas cerca.");
    Serial.println("      Presiona EN para recalibrar en unos minutos.");
  } else {
    Serial.println("   Calibracion OK.");
  }

  Serial.println();
  Serial.println("3) Midiendo. Acerca un encendedor SIN PRENDER y suelta gas 1-2 s.");
  Serial.println("----------------------------------------------");
}

void loop() {
  static unsigned long ultimo = 0;
  if (millis() - ultimo < PERIODO_MS) return;
  ultimo = millis();

  float vA0 = leerVoltajeA0();
  float rs = calcularRs(vA0);
  float ratio = rs / R0;
  float ppm = CURVA_A * pow(ratio, CURVA_B);
  float cambio = rs / rsAireLimpio;

  bool alerta = cambio < UMBRAL_ALERTA;
  digitalWrite(PIN_LED, alerta ? HIGH : LOW);

  Serial.printf("A0: %.2f V | Rs: %6.2f kOhm | Rs/R0: %5.2f | LP aprox: %6.0f ppm",
                vA0, rs, ratio, ppm);
  if (alerta) Serial.print("  <<< GAS DETECTADO");
  imprimirBarra(vA0, VCC_SENSOR);
}