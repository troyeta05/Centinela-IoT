/*
 * ============================================================
 *  PRUEBA INDIVIDUAL DEL SENSOR MQ-4 (Metano / Gas natural)
 *  Robot Centinela V7.2 - Banco de pruebas (circuito independiente)
 * ============================================================
 *
 *  El MQ-4 NECESITA 5V (su calentador interno no funciona con 3.3V),
 *  así que todo va del lado de VIN (parte de abajo a la izquierda):
 *
 *     MQ-4 (módulo)    ESP32 (NodeMCU-32S)
 *     -------------    -------------------
 *     VCC   ------->   VIN (5V del USB)
 *     GND   ------->   GND (pin /*
 * ============================================================
 *  PRUEBA INDIVIDUAL DEL SENSOR MQ-4 (Metano / Gas natural)
 *  Robot Centinela V7.2 - Banco de pruebas (circuito independiente)
 * ============================================================
 *
 *  El MQ-4 NECESITA 5V (su calentador interno no funciona con 3.3V),
 *  así que todo va del lado de VIN (parte de abajo a la izquierda):
 *
 *     MQ-4 (módulo)    ESP32 (NodeMCU-32S)
 *     -------------    -------------------
 *     VCC   ------->   VIN (5V del USB)
 *     GND   ------->   GND (pin entre D12 y D13)
 *     A0    --[10kΩ]--+-->  D12 (GPIO 32)
 *                     |
 *                   [20kΩ]
 *                     |
 *                    GND
 *     D0    ------->   sin conectar
 *
 *  El divisor 10k/20k es OBLIGATORIO: A0 puede subir hasta casi 5V
 *  con mucho gas y el ESP32 solo aguanta 3.3V. El divisor deja 2/3
 *  del voltaje (5V -> 3.33V). Si no tienes de 20kΩ, usa dos de 10kΩ
 *  en serie.
 *
 *  ¿Por qué D13? Es entrada analógica, está pegado a un GND y cerca
 *  de VIN. Funciona porque en esta prueba NO se usa Wi-Fi (los pines
 *  analógicos de este lado dejan de leer cuando el Wi-Fi está activo).
 *
 *  IMPORTANTE:
 *   - El sensor se CALIENTA, es normal. No lo toques por arriba.
 *   - La primera vez que se usa, un MQ nuevo necesita varias horas
 *     encendido para estabilizarse. Para esta prueba basta con
 *     el precalentamiento de abajo, pero las lecturas irán cambiando.
 *   - Las ppm son una ESTIMACIÓN (curva del datasheet). Sirven para
 *     ver que el sensor responde, no como medición certificada.
 *
 *  CÓMO PROBARLO:
 *   Acerca un encendedor SIN PRENDERLO y suelta un poco de gas cerca
 *   del sensor (1-2 s). El MQ-4 es para metano, pero también reacciona
 *   al gas del encendedor (butano). La lectura debe subir y luego bajar
 *   sola en uno o dos minutos. Nunca uses flama cerca del sensor.
 *
 *  Monitor Serie a 115200 baudios.
 *  LED azul integrado (GPIO 2): se enciende al detectar gas.
 * ============================================================
 */

// ---------------- CONFIGURACIÓN ----------------
const int PIN_A0  = 32;     // D32 (a través del divisor)
const int PIN_LED = 2;      // LED integrado

const float FACTOR_DIVISOR = 1.5;    // (10k + 20k) / 20k
const float VCC_SENSOR     = 5.0;    // Voltaje del calentador / circuito del MQ
const float RL_KOHM        = 10.0;   // Resistencia de carga del módulo:
                                     //   busca en la placa una resistencia
                                     //   marcada "103" = 10k  o  "102" = 1k
const float RS_R0_AIRE     = 4.4;    // Relación Rs/R0 en aire limpio (datasheet MQ-4)

// Curva CH4 del datasheet: ppm = A * (Rs/R0)^B
const float CURVA_A = 1012.7;
const float CURVA_B = -2.786;

const int   PRECALENTAMIENTO_S = 60;     // Segundos de calentamiento antes de calibrar
const int   MUESTRAS_CALIB     = 50;     // Lecturas para calcular R0
const float UMBRAL_ALERTA      = 0.70;   // Alerta si Rs cae al 70% de su valor en aire limpio
const unsigned long PERIODO_MS = 1000;
// ------------------------------------------------

float R0 = 0;
float rsAireLimpio = 0;

// Promedia varias lecturas y devuelve el voltaje REAL en A0 (antes del divisor)
float leerVoltajeA0() {
  long suma = 0;
  const int n = 20;
  for (int i = 0; i < n; i++) {
    suma += analogReadMilliVolts(PIN_A0);
    delay(2);
  }
  float vPin = (suma / (float)n) / 1000.0;   // Voltaje en el pin del ESP32
  return vPin * FACTOR_DIVISOR;              // Voltaje real que entrega el MQ
}

// Resistencia del sensor (kΩ) a partir del voltaje en A0
float calcularRs(float vA0) {
  if (vA0 < 0.01) vA0 = 0.01;                // Evita dividir entre cero
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
  Serial.println("      PRUEBA DE SENSOR MQ-4 (METANO / GAS)");
  Serial.println("==============================================");
  Serial.println("VCC -> VIN | GND -> GND | A0 -> divisor -> D13");
  Serial.println();

  // ---- 1. Precalentamiento ----
  Serial.printf("1) Precalentando %d s (deja el sensor en aire limpio)...\n", PRECALENTAMIENTO_S);
  for (int s = PRECALENTAMIENTO_S; s > 0; s--) {
    if (s % 10 == 0 || s <= 5) {
      float v = leerVoltajeA0();
      Serial.printf("   %3d s restantes | A0 = %.2f V\n", s, v);
    }
    digitalWrite(PIN_LED, s % 2);          // Parpadea mientras calienta
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

  // Diagnóstico de la calibración
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
  float cambio = rs / rsAireLimpio;          // 1.0 = igual que aire limpio

  bool alerta = cambio < UMBRAL_ALERTA;
  digitalWrite(PIN_LED, alerta ? HIGH : LOW);

  Serial.printf("A0: %.2f V | Rs: %6.2f kOhm | Rs/R0: %5.2f | CH4 aprox: %6.0f ppm",
                vA0, rs, ratio, ppm);
  if (alerta) Serial.print("  <<< GAS DETECTADO");
  imprimirBarra(vA0, VCC_SENSOR);
}

void loop() {
  static unsigned long ultimo = 0;
  if (millis() - ultimo < PERIODO_MS) return;
  ultimo = millis();

  float vA0 = leerVoltajeA0();
  float rs = calcularRs(vA0);
  float ratio = rs / R0;
  float ppm = CURVA_A * pow(ratio, CURVA_B);
  float cambio = rs / rsAireLimpio;          // 1.0 = igual que aire limpio

  bool alerta = cambio < UMBRAL_ALERTA;
  digitalWrite(PIN_LED, alerta ? HIGH : LOW);

  Serial.printf("A0: %.2f V | Rs: %6.2f kOhm | Rs/R0: %5.2f | CH4 aprox: %6.0f ppm",
                vA0, rs, ratio, ppm);
  if (alerta) Serial.print("  <<< GAS DETECTADO");
  imprimirBarra(vA0, VCC_SENSOR);
}
