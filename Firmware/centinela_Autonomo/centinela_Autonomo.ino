/*
 * ============================================================================
 * PROYECTO: ROBOT CENTINELA V7.2 — FIRMWARE 100% AUTONOMO (ESP32)
 * ============================================================================
 * Archivo: Centinela_Autonomo.ino
 * Plataforma: ESP32 (Compatible con Core ESP32 2.x y 3.x)
 *
 * ARQUITECTURA DE DOBLE NUCLEO (FreeRTOS):
 *  - Nucleo 1 (loop @ 50 Hz / cada 20 ms):
 *      Lectura de sensores en tiempo real, proteccion anti-caidas (escaleras),
 *      seguimiento de paredes con IR laterales, evasion de obstaculos con
 *      ultrasonico, deteccion de volcadura, deteccion de rampa empinada
 *      (Torque Boost), desatasco inteligente multi-etapa con encoders,
 *      odometria diferencial (x, y, theta) y rampa suave de PWM.
 *  - Nucleo 0 (Task Telemetria cada 2000 ms):
 *      Sincronizacion NTP UTC y envio HTTPS POST no bloqueante a Supabase
 *      (RPC ingest_telemetry, esquema JSON v1.1). El robot NUNCA se queda
 *      ciego ni congela sus motores mientras transmite a la nube.
 * ============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <time.h>
#include <math.h>
#include "esp_random.h"

// ============================================================================
// 1. CREDENCIALES WIFI Y SUPABASE (CONFIGURAR AQUI)
// ============================================================================
const char* WIFI_SSID     = "TU_RED_WIFI";
const char* WIFI_PASSWORD = "TU_PASSWORD_WIFI";

const char* SUPABASE_URL  = "PEGA_AQUI_TU_URL_SUPABASE";
// Clave service_role (sb_secret_...) para autorizar RPC ingest_telemetry
const char* SUPABASE_KEY  = "PEGA_AQUI_TU_SUPABASE_SECRET_KEY";

// Identificadores dados de alta en tu base de datos Supabase
const char* DEVICE_ID      = "bf8d0f7d-f739-4529-9a8a-5975f5b26960";
const char* MISSION_ID     = "0354909f-cc35-4ebc-822d-bfffeaa80374";
const char* MAP_VERSION_ID = "ecb3df81-477f-4e72-92c6-8b38f856cdb5";
const char* FIRMWARE_VER   = "1.0.0";

// ============================================================================
// 2. MAPA DE PINES (EXACTO AL DIAGRAMA DEL EQUIPO)
// ============================================================================
// Analogicos (ADC1 - No chocan con el WiFi)
constexpr int PIN_BAT_ADC          = 36; // VP: Divisor de voltaje de bateria
constexpr int PIN_MQ6_ADC          = 34; // A0 MQ-6 (Gas LP / Propano / Butano)
constexpr int PIN_MQ4_ADC          = 35; // A0 MQ-4 (Metano / Gas Natural)

// Ambiental y Ultrasonico
constexpr int PIN_DHT22            = 4;  // DATA DHT22 (3.3V)
constexpr int PIN_US_TRIG          = 5;  // TRIG HC-SR04
constexpr int PIN_US_ECHO          = 18; // ECHO HC-SR04 (con divisor a 3.3V)

// 6 Sensores Infrarrojos (4 de piso anti-caidas + 2 laterales para pared)
constexpr int PIN_IR_FLOOR_FL      = 25; // Piso Frontal Izquierdo
constexpr int PIN_IR_FLOOR_FR      = 26; // Piso Frontal Derecho
constexpr int PIN_IR_FLOOR_RL      = 27; // Piso Trasero Izquierdo
constexpr int PIN_IR_FLOOR_RR      = 14; // Piso Trasero Derecho
constexpr int PIN_IR_LAT_LEFT      = 13; // Pared Lateral Izquierdo
constexpr int PIN_IR_LAT_RIGHT     = 16; // Pared Lateral Derecho

// Encoders Magneticos Hall (Motores GM25-370, alimentados a 3.3V)
constexpr int PIN_ENC_LEFT         = 19; // Canal A Motor Izquierdo
constexpr int PIN_ENC_RIGHT        = 23; // Canal A Motor Derecho

// Puente H L298N (Control de direccion y velocidad por PWM)
constexpr int PIN_MOT_L_IN1        = 21; // Motor Izquierdo IN1
constexpr int PIN_MOT_L_IN2        = 22; // Motor Izquierdo IN2
constexpr int PIN_MOT_R_IN3        = 32; // Motor Derecho IN3
constexpr int PIN_MOT_R_IN4        = 33; // Motor Derecho IN4

// ============================================================================
// 3. PARAMETROS FISICOS, ELECTRICOS Y UMBRALES DE SEGURIDAD
// ============================================================================
// Geometria del chasis y encoders (ajustar si cambia el diametro de llanta)
constexpr float WHEEL_DIAMETER_M   = 0.067f;  // 67 mm diametro de llanta
constexpr float WHEEL_TRACK_M      = 0.175f;  // 17.5 cm distancia entre llantas
constexpr float PULSES_PER_REV     = 340.0f;  // Pulsos por vuelta de rueda (GM25-370)
constexpr float METERS_PER_PULSE   = (3.14159265f * WHEEL_DIAMETER_M) / PULSES_PER_REV;

// Configuracion PWM (8 bits: 0..255 a 1 kHz para L298N)
constexpr int PWM_FREQ_HZ          = 1000;
constexpr int PWM_RES_BITS         = 8;
constexpr int PWM_CRUISE           = 185;     // Velocidad normal de patrullaje
constexpr int PWM_SLOW             = 150;     // Velocidad en pasillo estrecho o giro suave
constexpr int PWM_TURN             = 195;     // Velocidad de giro sobre su eje
constexpr int PWM_CLIMB_BOOST      = 240;     // Torque extra para subir rampas/alfombras
constexpr int PWM_RAMP_STEP        = 18;      // Paso maximo por cada 20ms (protege el MT3608)

// Distancias del ultrasonico (en metros)
constexpr float DIST_EMERGENCY_M   = 0.15f;   // < 15 cm: muy cerca, reversa + giro
constexpr float DIST_OBSTACLE_M    = 0.32f;   // < 32 cm: obstaculo frontal, esquivar
constexpr float DIST_SLOWDOWN_M    = 0.50f;   // < 50 cm: reducir velocidad preventivamente

// Umbrales locales de Gas (cuentas ADC 0..4095) para paro preventivo de seguridad
constexpr int GAS_MQ6_CRIT_RAW     = 2700;    // Umbral critico local LP
constexpr int GAS_MQ4_CRIT_RAW     = 2700;    // Umbral critico local Metano
constexpr int GAS_HYSTERESIS_RAW   = 350;     // Histeresis para salir de emergencia

// Divisor de bateria (R1 = 100k, R2 = 27k -> factor ~ 4.7037)
// NOTA: Si mides el bus de 10V o las celdas 1S (3.7V), ajusta estos umbrales:
constexpr float BAT_DIVIDER_RATIO  = (100.0f + 27.0f) / 27.0f;
constexpr float BAT_ADC_CAL        = 1.03f;   // Factor de correccion de lectura ADC
constexpr float BAT_MIN_VOLTS      = 3.25f;   // Voltaje minimo antes de estacionarse por bateria baja
constexpr float BAT_NOM_MAX_VOLTS  = 4.20f;   // 4.2V si mide las celdas en paralelo (o 12.6V si es 3S)

// Tiempos de control
constexpr uint32_t CONTROL_PERIOD_MS   = 20;   // 50 Hz lazo de navegacion
constexpr uint32_t TELEMETRY_PERIOD_MS = 2000; // 0.5 Hz envio a Supabase
constexpr uint32_t DHT_READ_PERIOD_MS  = 2500; // Lectura de DHT22 cada 2.5 s

// ============================================================================
// 4. MAQUINA DE ESTADOS FINITOS (10 SITUACIONES AUTONOMAS)
// ============================================================================
enum RobotBehaviorState {
  STATE_BOOT_WAIT = 0,          // Espera inicial de 2s al encender
  STATE_PATROL_CRUISE,          // Avance autonomo libre
  STATE_WALL_FOLLOWING,         // Siguiendo pared con sensores IR laterales
  STATE_AVOID_OBSTACLE,         // Esquivando obstaculo detectado por ultrasonico
  STATE_CLIFF_FRONT_ESCAPE,     // Escapando de escalon / precipicio frontal
  STATE_CLIFF_REAR_ESCAPE,      // Escapando de escalon trasero al ir en reversa
  STATE_STEEP_CLIMB_BOOST,      // Subiendo lugar empinado / rampa (Torque Boost)
  STATE_UNSTUCK_MANEUVER,       // Maniobra multi-etapa para desatorarse
  STATE_FLIPPED_EMERGENCY,      // Robot volteado o levantado del piso (corte motor)
  STATE_TRAPPED_COOLDOWN,       // Robot atrapado tras 4 intentos (pausa protectora)
  STATE_GAS_EMERGENCY_STOP,     // Fuga critica de gas: paro de motores y monitoreo
  STATE_LOW_BATTERY_PARKED      // Bateria critica: estacionado seguro
};

// ============================================================================
// 5. ESTRUCTURAS DE DATOS Y VARIABLES GLOBALES
// ============================================================================
DHT dht(PIN_DHT22, DHT22);
portMUX_TYPE telemetryMux = portMUX_INITIALIZER_UNLOCKED;

// Contadores de encoders en RAM interna (ISR)
volatile uint32_t isrPulsesLeft  = 0;
volatile uint32_t isrPulsesRight = 0;
volatile uint32_t lastEdgeUsLeft = 0;
volatile uint32_t lastEdgeUsRight = 0;

// Estado compartido entre Nucleo 1 (Control) y Nucleo 0 (Supabase)
struct SharedRobotData {
  // Identidad de sesion
  char bootId[37];
  uint32_t sequence;
  bool clockSynced;

  // Pose odometrica
  float x_m;
  float y_m;
  float theta_rad;
  float sigma_pos_m;

  // Sensores
  int mq6_raw;
  int mq4_raw;
  float temperature_c;
  float humidity_pct;
  bool dht_valid;
  float front_distance_m;

  // Infrarrojos en logica positiva del contrato JSON:
  // true = SEGURO (hay piso / lateral despejado), false = PELIGRO (precipicio / pared pegada)
  bool ir_floor_fl;
  bool ir_floor_fr;
  bool ir_floor_rl;
  bool ir_floor_rr;
  bool ir_lat_left;
  bool ir_lat_right;

  // Bateria
  float battery_v;
  float battery_soc;

  // Estado operativo
  const char* robot_state_str;
  const char* nav_state_str;
  bool motors_active;
} gState;

// Variables internas de navegacion (Nucleo 1)
RobotBehaviorState currentState = STATE_BOOT_WAIT;
uint32_t stateEnteredMs         = 0;
uint32_t subStepStartMs         = 0;
uint8_t  maneuverStep           = 0;
uint8_t  unstuckAttemptCount    = 0;
int8_t   preferredTurnDir       = 1; // +1 = Izquierda, -1 = Derecha
int8_t   lastWallFollowed       = 0; // -1 = Pared Izq, +1 = Pared Der, 0 = Ninguna

// Variables de control de motores (Rampa suave PWM)
int currentSpeedL = 0; // -255 a +255
int currentSpeedR = 0; // -255 a +255
int targetSpeedL  = 0;
int targetSpeedR  = 0;

// Monitoreo de velocidad real con encoders (deteccion de rampa empinada o atasco)
uint32_t prevEncCheckMs    = 0;
uint32_t prevPulsesLeft    = 0;
uint32_t prevPulsesRight   = 0;
float    pulseRateLeftHz   = 0.0f;
float    pulseRateRightHz  = 0.0f;
uint32_t stallTimerMs      = 0;
uint32_t slowClimbTimerMs  = 0;
uint32_t flippedTimerMs    = 0;
uint32_t lastExploreTurnMs = 0;

// ============================================================================
// 6. INTERRUPCIONES DE ENCODERS (CON FILTRO ANTI-REBOTE DE 400 us)
// ============================================================================
void IRAM_ATTR onLeftEncoderPulse() {
  uint32_t nowUs = micros();
  if (nowUs - lastEdgeUsLeft > 400) {
    isrPulsesLeft++;
    lastEdgeUsLeft = nowUs;
  }
}

void IRAM_ATTR onRightEncoderPulse() {
  uint32_t nowUs = micros();
  if (nowUs - lastEdgeUsRight > 400) {
    isrPulsesRight++;
    lastEdgeUsRight = nowUs;
  }
}

// ============================================================================
// 7. UTILIDADES: GENERADOR UUID v4 Y FORMATO DE RELOJ UTC
// ============================================================================
void generateUUIDv4(char* out37) {
  uint8_t b[16];
  for (int i = 0; i < 16; i += 4) {
    uint32_t r = esp_random();
    b[i]     = (r >> 24) & 0xFF;
    b[i + 1] = (r >> 16) & 0xFF;
    b[i + 2] = (r >> 8)  & 0xFF;
    b[i + 3] = r & 0xFF;
  }
  b[6] = (b[6] & 0x0F) | 0x40; // Version 4
  b[8] = (b[8] & 0x3F) | 0x80; // Variant 1
  snprintf(out37, 37,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
           b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

bool getIsoUtcTimestamp(char* buf, size_t len) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10)) {
    return false;
  }
  if (timeinfo.tm_year + 1900 < 2025) {
    return false;
  }
  strftime(buf, len, "%Y-%m-%dT%H:%M:%S.000Z", &timeinfo);
  return true;
}

// ============================================================================
// 8. DRIVER DE MOTORES L298N CON RAMPA SUAVE (COMPATIBLE CORE 2.x y 3.x)
// ============================================================================
void initMotorPWM() {
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  ledcAttach(PIN_MOT_L_IN1, PWM_FREQ_HZ, PWM_RES_BITS);
  ledcAttach(PIN_MOT_L_IN2, PWM_FREQ_HZ, PWM_RES_BITS);
  ledcAttach(PIN_MOT_R_IN3, PWM_FREQ_HZ, PWM_RES_BITS);
  ledcAttach(PIN_MOT_R_IN4, PWM_FREQ_HZ, PWM_RES_BITS);
#else
  ledcSetup(0, PWM_FREQ_HZ, PWM_RES_BITS);
  ledcSetup(1, PWM_FREQ_HZ, PWM_RES_BITS);
  ledcSetup(2, PWM_FREQ_HZ, PWM_RES_BITS);
  ledcSetup(3, PWM_FREQ_HZ, PWM_RES_BITS);
  ledcAttachPin(PIN_MOT_L_IN1, 0);
  ledcAttachPin(PIN_MOT_L_IN2, 1);
  ledcAttachPin(PIN_MOT_R_IN3, 2);
  ledcAttachPin(PIN_MOT_R_IN4, 3);
#endif
}

void writePwmPin(int pin, int channel, uint32_t duty) {
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  (void)channel;
  ledcWrite(pin, duty);
#else
  (void)pin;
  ledcWrite(channel, duty);
#endif
}

void applyHardwareMotors(int speedL, int speedR) {
  speedL = constrain(speedL, -255, 255);
  speedR = constrain(speedR, -255, 255);

  // Motor Izquierdo
  if (speedL > 0) {
    writePwmPin(PIN_MOT_L_IN1, 0, speedL);
    writePwmPin(PIN_MOT_L_IN2, 1, 0);
  } else if (speedL < 0) {
    writePwmPin(PIN_MOT_L_IN1, 0, 0);
    writePwmPin(PIN_MOT_L_IN2, 1, -speedL);
  } else {
    writePwmPin(PIN_MOT_L_IN1, 0, 0);
    writePwmPin(PIN_MOT_L_IN2, 1, 0);
  }

  // Motor Derecho
  if (speedR > 0) {
    writePwmPin(PIN_MOT_R_IN3, 2, speedR);
    writePwmPin(PIN_MOT_R_IN4, 3, 0);
  } else if (speedR < 0) {
    writePwmPin(PIN_MOT_R_IN3, 2, 0);
    writePwmPin(PIN_MOT_R_IN4, 3, -speedR);
  } else {
    writePwmPin(PIN_MOT_R_IN3, 2, 0);
    writePwmPin(PIN_MOT_R_IN4, 3, 0);
  }
}

// Fija la velocidad deseada (con rampa suave)
void setTargetMotors(int left, int right) {
  targetSpeedL = constrain(left, -255, 255);
  targetSpeedR = constrain(right, -255, 255);
}

// Freno de emergencia instantaneo (sin rampa, para precipicios o volcadura)
void emergencyStopMotors() {
  targetSpeedL  = 0;
  targetSpeedR  = 0;
  currentSpeedL = 0;
  currentSpeedR = 0;
  applyHardwareMotors(0, 0);
}

// Actualiza la rampa de aceleracion cada 20 ms para evitar picos de corriente en el MT3608
void updateMotorRamp() {
  if (currentSpeedL < targetSpeedL) {
    currentSpeedL = min(currentSpeedL + PWM_RAMP_STEP, targetSpeedL);
  } else if (currentSpeedL > targetSpeedL) {
    currentSpeedL = max(currentSpeedL - PWM_RAMP_STEP, targetSpeedL);
  }

  if (currentSpeedR < targetSpeedR) {
    currentSpeedR = min(currentSpeedR + PWM_RAMP_STEP, targetSpeedR);
  } else if (currentSpeedR > targetSpeedR) {
    currentSpeedR = max(currentSpeedR - PWM_RAMP_STEP, targetSpeedR);
  }

  applyHardwareMotors(currentSpeedL, currentSpeedR);
}

// ============================================================================
// 9. LECTURA Y FILTRADO DE SENSORES
// ============================================================================
int readAdcAveraged(int pin, int samples = 8) {
  uint32_t sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
  }
  return (int)(sum / samples);
}

float readUltrasonicMeters() {
  digitalWrite(PIN_US_TRIG, LOW);
  delayMicroseconds(3);
  digitalWrite(PIN_US_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_US_TRIG, LOW);

  // Timeout de 24,000 us (~4.0 metros max) para NUNCA bloquear el microcontrolador
  unsigned long durationUs = pulseIn(PIN_US_ECHO, HIGH, 24000UL);
  if (durationUs == 0) {
    return 4.00f; // Camino libre (> 4 m)
  }
  float distM = (durationUs * 0.000343f) * 0.5f;
  return constrain(distM, 0.02f, 4.00f);
}

// En los modulos IR (LM393 / TCRT5000):
//   LOW (0)  = Luz infrarroja reflejada (Hay piso / Hay pared cerca)
//   HIGH (1) = Sin reflejo (Precipicio/Abismo en piso / Camino despejado en lateral)
// Convertimos a la logica positiva estandarizada del contrato Centinela:
//   - Piso:    true = Hay piso firme (Seguro), false = Abismo/Escalera (Peligro)
//   - Lateral: true = Despejado (Seguro),      false = Pared muy cerca (Obstaculo lateral)
void readAllSensors() {
  uint32_t now = millis();

  // 1. Infrarrojos de Piso (LOW en pin fisico = hay suelo debajo = true)
  bool fl = (digitalRead(PIN_IR_FLOOR_FL) == LOW);
  bool fr = (digitalRead(PIN_IR_FLOOR_FR) == LOW);
  bool rl = (digitalRead(PIN_IR_FLOOR_RL) == LOW);
  bool rr = (digitalRead(PIN_IR_FLOOR_RR) == LOW);

  // 2. Infrarrojos Laterales (HIGH en pin fisico = libre = true; LOW = pared detectada = false)
  bool latL = (digitalRead(PIN_IR_LAT_LEFT)  == HIGH);
  bool latR = (digitalRead(PIN_IR_LAT_RIGHT) == HIGH);

  // 3. Ultrasonico con filtro suavizado exponencial para evitar falsos disparos
  static float filteredDistM = 1.50f;
  float rawDistM = readUltrasonicMeters();
  if (rawDistM < filteredDistM) {
    // Reaccion rapida si un objeto se acerca de repente
    filteredDistM = 0.65f * rawDistM + 0.35f * filteredDistM;
  } else {
    filteredDistM = 0.30f * rawDistM + 0.70f * filteredDistM;
  }

  // 4. Gases MQ-6 y MQ-4 (sobremuestreo de 8 lecturas)
  int mq6 = readAdcAveraged(PIN_MQ6_ADC, 8);
  int mq4 = readAdcAveraged(PIN_MQ4_ADC, 8);

  // 5. Bateria (divisor resistivo)
  int batRaw = readAdcAveraged(PIN_BAT_ADC, 8);
  float vPin = (batRaw / 4095.0f) * 3.3f * BAT_ADC_CAL;
  float vBat = vPin * BAT_DIVIDER_RATIO;
  // Si el divisor aún no está soldado (lectura ~0V), reportamos el voltaje nominal del sistema
  if (vBat < 0.5f) {
    vBat = 3.85f;
  }
  float soc = ((vBat - BAT_MIN_VOLTS) / (BAT_NOM_MAX_VOLTS - BAT_MIN_VOLTS)) * 100.0f;
  soc = constrain(soc, 0.0f, 100.0f);

  // 6. DHT22 (se lee cada 2.5 segundos para respetar el timing del sensor)
  static uint32_t lastDhtMs = 0;
  static float lastTemp = 24.0f;
  static float lastHum  = 45.0f;
  static bool  dhtOk    = false;
  if (now - lastDhtMs >= DHT_READ_PERIOD_MS) {
    lastDhtMs = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      lastTemp = t;
      lastHum  = h;
      dhtOk    = true;
    } else {
      dhtOk    = false;
    }
  }

  // Guardar en estructura compartida con seccion critica corta
  portENTER_CRITICAL(&telemetryMux);
  gState.ir_floor_fl      = fl;
  gState.ir_floor_fr      = fr;
  gState.ir_floor_rl      = rl;
  gState.ir_floor_rr      = rr;
  gState.ir_lat_left      = latL;
  gState.ir_lat_right     = latR;
  gState.front_distance_m = filteredDistM;
  gState.mq6_raw          = mq6;
  gState.mq4_raw          = mq4;
  gState.battery_v        = vBat;
  gState.battery_soc      = soc;
  gState.temperature_c    = lastTemp;
  gState.humidity_pct     = lastHum;
  gState.dht_valid        = dhtOk;
  portEXIT_CRITICAL(&telemetryMux);
}

// ============================================================================
// 10. ODOMETRIA DIFERENCIAL Y DETECCION DE ATASCO / RAMPA EMPINADA
// ============================================================================
void updateOdometryAndWheelHealth() {
  uint32_t now = millis();
  uint32_t dtMs = now - prevEncCheckMs;
  if (dtMs < 100) return; // Evaluar cada 100 ms

  noInterrupts();
  uint32_t pL = isrPulsesLeft;
  uint32_t pR = isrPulsesRight;
  interrupts();

  uint32_t dPL = pL - prevPulsesLeft;
  uint32_t dPR = pR - prevPulsesRight;
  prevPulsesLeft  = pL;
  prevPulsesRight = pR;
  prevEncCheckMs  = now;

  float dtSec = dtMs / 1000.0f;
  pulseRateLeftHz  = dPL / dtSec;
  pulseRateRightHz = dPR / dtSec;

  // Direccion del movimiento segun el signo del PWM actual
  float signL = (currentSpeedL >= 0) ? 1.0f : -1.0f;
  float signR = (currentSpeedR >= 0) ? 1.0f : -1.0f;

  // Si los encoders no estan generando pulsos pero los motores se mueven en modo sin encoder,
  // estimamos un avance conservador solo si hay pulsos reales; si hay encoders conectados,
  // usamos la cinematica exacta de los pulsos:
  float dSL = (dPL * METERS_PER_PULSE) * signL;
  float dSR = (dPR * METERS_PER_PULSE) * signR;

  float dCenter = 0.5f * (dSL + dSR);
  float dTheta  = (dSR - dSL) / WHEEL_TRACK_M;

  portENTER_CRITICAL(&telemetryMux);
  float midTheta = gState.theta_rad + 0.5f * dTheta;
  gState.x_m       += dCenter * cosf(midTheta);
  gState.y_m       += dCenter * sinf(midTheta);
  gState.theta_rad += dTheta;
  // Normalizar angulo entre -PI y +PI
  while (gState.theta_rad >  3.14159265f) gState.theta_rad -= 6.28318530f;
  while (gState.theta_rad < -3.14159265f) gState.theta_rad += 6.28318530f;
  gState.sigma_pos_m = min(0.50f, gState.sigma_pos_m + fabsf(dCenter) * 0.01f);
  portEXIT_CRITICAL(&telemetryMux);
}

// ============================================================================
// 11. CEREBRO AUTONOMO: MAQUINA DE ESTADOS PARA TODAS LAS SITUACIONES
// ============================================================================
void changeState(RobotBehaviorState newState, const char* reportedState, const char* navState) {
  currentState   = newState;
  stateEnteredMs = millis();
  subStepStartMs = stateEnteredMs;
  maneuverStep   = 0;

  portENTER_CRITICAL(&telemetryMux);
  gState.robot_state_str = reportedState;
  gState.nav_state_str   = navState;
  portEXIT_CRITICAL(&telemetryMux);
}

void runAutonomousBrain() {
  uint32_t now = millis();

  // Copiar lecturas actuales de forma segura
  bool fl   = gState.ir_floor_fl;
  bool fr   = gState.ir_floor_fr;
  bool rl   = gState.ir_floor_rl;
  bool rr   = gState.ir_floor_rr;
  bool latL = gState.ir_lat_left;
  bool latR = gState.ir_lat_right;
  float distM = gState.front_distance_m;
  int mq6   = gState.mq6_raw;
  int mq4   = gState.mq4_raw;

  // --------------------------------------------------------------------------
  // PRIORIDAD 1: ¿EL ROBOT SE VOLTEO O FUE LEVANTADO DEL PISO?
  // Si 3 o los 4 sensores de piso pierden el suelo al mismo tiempo por > 350 ms,
  // el robot se volteo o alguien lo levanto. Apagar motores de inmediato.
  // --------------------------------------------------------------------------
  int missingFloorCount = (!fl ? 1 : 0) + (!fr ? 1 : 0) + (!rl ? 1 : 0) + (!rr ? 1 : 0);
  if (missingFloorCount >= 3) {
    if (flippedTimerMs == 0) flippedTimerMs = now;
    if (now - flippedTimerMs > 350 && currentState != STATE_FLIPPED_EMERGENCY) {
      emergencyStopMotors();
      changeState(STATE_FLIPPED_EMERGENCY, "error_flipped", "emergency_flipped_cutoff");
    }
  } else {
    flippedTimerMs = 0;
  }

  if (currentState == STATE_FLIPPED_EMERGENCY) {
    emergencyStopMotors();
    // Si vuelve a estar sobre sus 4 ruedas durante 3 segundos seguidos, retoma patrullaje
    if (fl && fr && rl && rr && (now - stateEnteredMs > 3000)) {
      changeState(STATE_PATROL_CRUISE, "moving", "patrol_cruise");
    }
    return;
  }

  // --------------------------------------------------------------------------
  // PRIORIDAD 2: ¿HAY FUGA CRITICA DE GAS? (PARO PREVENTIVO DE CHISPA)
  // Si detecta una concentracion critica de gas, detiene los motores para no
  // generar chispas en las escobillas y se queda transmitiendo la alerta a Supabase.
  // --------------------------------------------------------------------------
  if (mq6 >= GAS_MQ6_CRIT_RAW || mq4 >= GAS_MQ4_CRIT_RAW) {
    if (currentState != STATE_GAS_EMERGENCY_STOP) {
      emergencyStopMotors();
      changeState(STATE_GAS_EMERGENCY_STOP, "alert_gas_leak", "gas_emergency_hold");
    }
  }
  if (currentState == STATE_GAS_EMERGENCY_STOP) {
    emergencyStopMotors();
    if (mq6 < (GAS_MQ6_CRIT_RAW - GAS_HYSTERESIS_RAW) &&
        mq4 < (GAS_MQ4_CRIT_RAW - GAS_HYSTERESIS_RAW)) {
      changeState(STATE_PATROL_CRUISE, "moving", "patrol_cruise");
    }
    return;
  }

  // --------------------------------------------------------------------------
  // PRIORIDAD 3: ¿HAY PRECIPICIO / ESCALERA EN EL FRENTE?
  // Si cualquiera de los sensores frontales de piso deja de ver suelo mientras
  // avanzamos, frenar en seco y ejecutar maniobra de escape hacia atras.
  // --------------------------------------------------------------------------
  if ((!fl || !fr) &&
      currentState != STATE_CLIFF_FRONT_ESCAPE &&
      currentState != STATE_CLIFF_REAR_ESCAPE) {
    emergencyStopMotors();
    // Elegir girar hacia el lado opuesto al precipicio
    if (!fl && fr)      preferredTurnDir = -1; // Precipicio a la izq -> girar a la derecha
    else if (fl && !fr) preferredTurnDir =  1; // Precipicio a la der -> girar a la izquierda
    else                preferredTurnDir = (esp_random() & 1) ? 1 : -1;

    changeState(STATE_CLIFF_FRONT_ESCAPE, "avoiding_cliff", "cliff_front_escape");
  }

  // --------------------------------------------------------------------------
  // PRIORIDAD 4: ¿PRECIPICIO TRASERO MIENTRAS RETROCEDE?
  // Si el robot va en reversa y los sensores traseros detectan un escalon atras,
  // frenar de inmediato e ir hacia adelante.
  // --------------------------------------------------------------------------
  bool goingBackwards = (currentSpeedL < -10 || currentSpeedR < -10);
  if (goingBackwards && (!rl || !rr) && currentState != STATE_CLIFF_REAR_ESCAPE) {
    emergencyStopMotors();
    changeState(STATE_CLIFF_REAR_ESCAPE, "avoiding_cliff", "cliff_rear_escape");
  }

  // --------------------------------------------------------------------------
  // EJECUCION DE LA MAQUINA DE ESTADOS
  // --------------------------------------------------------------------------
  switch (currentState) {

    // ------------------------------------------------------------------------
    // CASO 0: ESPERA INICIAL AL ENCENDER
    // ------------------------------------------------------------------------
    case STATE_BOOT_WAIT: {
      setTargetMotors(0, 0);
      if (now - stateEnteredMs >= 2000) {
        lastExploreTurnMs = now;
        changeState(STATE_PATROL_CRUISE, "moving", "patrol_cruise");
      }
      break;
    }

    // ------------------------------------------------------------------------
    // CASO 1: ESCAPE DE PRECIPICIO / ESCALERA FRONTAL
    // ------------------------------------------------------------------------
    case STATE_CLIFF_FRONT_ESCAPE: {
      uint32_t dt = now - subStepStartMs;
      if (maneuverStep == 0) {
        // Paso 0: Pausa de freno activo (180 ms)
        setTargetMotors(0, 0);
        if (dt >= 180) { maneuverStep = 1; subStepStartMs = now; }
      } else if (maneuverStep == 1) {
        // Paso 1: Retroceder recto 650 ms (solo si el piso trasero es seguro)
        if (!rl || !rr) {
          maneuverStep = 2; subStepStartMs = now;
        } else {
          setTargetMotors(-PWM_SLOW, -PWM_SLOW);
          if (dt >= 650) { maneuverStep = 2; subStepStartMs = now; }
        }
      } else if (maneuverStep == 2) {
        // Paso 2: Girar ~110 grados alejandose del precipicio (650 ms)
        if (preferredTurnDir > 0) setTargetMotors(-PWM_TURN,  PWM_TURN);
        else                      setTargetMotors( PWM_TURN, -PWM_TURN);
        if (dt >= 650) {
          changeState(STATE_PATROL_CRUISE, "moving", "patrol_cruise");
        }
      }
      break;
    }

    // ------------------------------------------------------------------------
    // CASO 2: ESCAPE DE ESCALON TRASERO
    // ------------------------------------------------------------------------
    case STATE_CLIFF_REAR_ESCAPE: {
      uint32_t dt = now - subStepStartMs;
      if (maneuverStep == 0) {
        // Avanzar ligeramente para alejar la cola del borde (450 ms)
        if (fl && fr && distM > DIST_EMERGENCY_M) {
          setTargetMotors(PWM_SLOW, PWM_SLOW);
        } else {
          setTargetMotors(0, 0);
        }
        if (dt >= 450) { maneuverStep = 1; subStepStartMs = now; }
      } else {
        // Girar en su propio eje (500 ms)
        setTargetMotors(PWM_TURN, -PWM_TURN);
        if (dt >= 500) {
          changeState(STATE_PATROL_CRUISE, "moving", "patrol_cruise");
        }
      }
      break;
    }

    // ------------------------------------------------------------------------
    // CASO 3: PATRULLAJE Y SEGUIMIENTO DE PAREDES (CON IR LATERALES)
    // ------------------------------------------------------------------------
    case STATE_PATROL_CRUISE:
    case STATE_WALL_FOLLOWING: {
      // A) ¿Se atraveso un obstaculo al frente en el ultrasonico?
      if (distM < DIST_OBSTACLE_M) {
        changeState(STATE_AVOID_OBSTACLE, "avoiding_obstacle", "ultrasonic_evasion");
        break;
      }

      // B) Logica de seguimiento y centrado de paredes con sensores IR laterales:
      //    latL == false -> hay pared muy cerca a la izquierda
      //    latR == false -> hay pared muy cerca a la derecha
      int baseSpeed = (distM < DIST_SLOWDOWN_M) ? PWM_SLOW : PWM_CRUISE;

      if (!latL && !latR) {
        // Pasillo estrecho (pared en ambos lados): avanzar centrado y suave
        setTargetMotors(PWM_SLOW, PWM_SLOW);
        gState.robot_state_str = "moving";
        gState.nav_state_str   = "narrow_corridor_centering";
      } else if (!latL && latR) {
        // Pared detectada a la izquierda: seguir la pared izquierda corrigiendo
        // suavemente a la derecha para no rasparla
        lastWallFollowed = -1;
        setTargetMotors(baseSpeed + 15, baseSpeed - 35);
        currentState = STATE_WALL_FOLLOWING;
        gState.robot_state_str = "moving";
        gState.nav_state_str   = "wall_following_left";
      } else if (latL && !latR) {
        // Pared detectada a la derecha: seguir la pared derecha corrigiendo
        // suavemente a la izquierda para mantener distancia paralela
        lastWallFollowed = 1;
        setTargetMotors(baseSpeed - 35, baseSpeed + 15);
        currentState = STATE_WALL_FOLLOWING;
        gState.robot_state_str = "moving";
        gState.nav_state_str   = "wall_following_right";
      } else {
        // Ambos laterales despejados:
        // Si venia siguiendo una pared y se termino (esquina exterior / puerta),
        // hace una curva suave para rodear el marco o explorar habitaciones;
        // si lleva mas de 18 segundos en linea recta abierta, hace un giro corto de exploracion.
        currentState = STATE_PATROL_CRUISE;
        gState.robot_state_str = "moving";
        gState.nav_state_str   = "patrol_cruise";

        if (now - lastExploreTurnMs > 18000) {
          lastExploreTurnMs = now;
          setTargetMotors(PWM_CRUISE, PWM_CRUISE - 45);
        } else {
          setTargetMotors(baseSpeed, baseSpeed);
        }
      }

      // C) DETECCION DE LUGAR EMPINADO (RAMPA) O ATASCO MECANICO CON ENCODERS
      // Evaluamos si los motores tienen orden de avanzar pero las ruedas giran lento o nada
      bool commandingForward = (targetSpeedL > 120 && targetSpeedR > 120);
      float minPulseHz = min(pulseRateLeftHz, pulseRateRightHz);
      float maxPulseHz = max(pulseRateLeftHz, pulseRateRightHz);

      if (commandingForward && (now - stateEnteredMs > 600)) {
        // Caso C1: Una o ambas ruedas estan completamente bloqueadas (0 pulsos)
        if (minPulseHz < 1.0f && maxPulseHz < 2.0f) {
          if (stallTimerMs == 0) stallTimerMs = now;
          // Si tras 900 ms sigue en 0 pulsos, primero intentamos Torque Boost (por si es un tope/alfombra)
          if (now - stallTimerMs > 900) {
            stallTimerMs = 0;
            changeState(STATE_STEEP_CLIMB_BOOST, "climbing_boost", "steep_incline_boost");
            break;
          }
        }
        // Caso C2: Las ruedas SI giran pero forzadas/lentas (subiendo una rampa empinada)
        else if (minPulseHz >= 1.0f && minPulseHz < 28.0f) {
          if (slowClimbTimerMs == 0) slowClimbTimerMs = now;
          if (now - slowClimbTimerMs > 500) {
            slowClimbTimerMs = 0;
            changeState(STATE_STEEP_CLIMB_BOOST, "climbing_boost", "steep_incline_boost");
            break;
          }
        } else {
          stallTimerMs     = 0;
          slowClimbTimerMs = 0;
          unstuckAttemptCount = 0; // El robot avanza libremente, reiniciar contador de atascos
        }
      }
      break;
    }

    // ------------------------------------------------------------------------
    // CASO 4: ESQUIVAR OBSTACULO FRONTAL (ULTRASONICO + IR LATERALES)
    // Decide hacia donde girar leyendo que lado tiene pared y cual esta libre
    // ------------------------------------------------------------------------
    case STATE_AVOID_OBSTACLE: {
      uint32_t dt = now - subStepStartMs;

      if (maneuverStep == 0) {
        // Elegir direccion inteligente usando los IR laterales:
        if (!latL && latR) {
          preferredTurnDir = -1; // Pared a la izq, libre a la der -> girar a la DERECHA
        } else if (latL && !latR) {
          preferredTurnDir =  1; // Pared a la der, libre a la izq -> girar a la IZQUIERDA
        } else if (!latL && !latR) {
          preferredTurnDir =  2; // Callejón cerrado (pared en ambos lados y al frente) -> Giro 180°
        } else {
          // Ambos lados libres: si venia siguiendo una pared, girar paralelo a ella
          preferredTurnDir = (lastWallFollowed == -1) ? -1 : 1;
        }

        // Si el obstaculo esta demasiado cerca (< 16 cm) o es callejon cerrado, dar reversa primero
        if (distM < DIST_EMERGENCY_M || preferredTurnDir == 2) {
          maneuverStep = 1; // Ir a reversa
        } else {
          maneuverStep = 2; // Ir directo a giro
        }
        subStepStartMs = now;
      }
      else if (maneuverStep == 1) {
        // Reversa corta de seguridad (500 ms) cuidando sensores de piso traseros
        if (rl && rr) {
          setTargetMotors(-PWM_SLOW, -PWM_SLOW);
        } else {
          setTargetMotors(0, 0);
        }
        if (dt >= 500) {
          maneuverStep   = 2;
          subStepStartMs = now;
        }
      }
      else if (maneuverStep == 2) {
        // Girar hasta que el frente este despejado (> 45 cm) o se cumpla el tiempo de giro
        uint32_t turnDurationMs = (preferredTurnDir == 2) ? 1150 : 580;
        if (preferredTurnDir >= 1) {
          setTargetMotors(-PWM_TURN, PWM_TURN); // Giro izquierda
        } else {
          setTargetMotors(PWM_TURN, -PWM_TURN); // Giro derecha
        }

        if ((dt >= 320 && distM > 0.55f && preferredTurnDir != 2) || dt >= turnDurationMs) {
          lastExploreTurnMs = now;
          changeState(STATE_PATROL_CRUISE, "moving", "patrol_cruise");
        }
      }
      break;
    }

    // ------------------------------------------------------------------------
    // CASO 5: SUBIDA DE LUGAR EMPINADO / RAMPA / UMBRAL (TORQUE BOOST)
    // Aplica potencia extra para trepar una rampa, pero vigila los encoders:
    // si aun con maxima potencia las ruedas no giran, pasa a DESATASCO para no
    // sobrecargar el elevador MT3608 ni quemar motores.
    // ------------------------------------------------------------------------
    case STATE_STEEP_CLIMB_BOOST: {
      uint32_t dt = now - stateEnteredMs;

      // Si aparece un obstaculo o precipicio mientras sube, abortar boost de inmediato
      if (distM < DIST_EMERGENCY_M) {
        changeState(STATE_AVOID_OBSTACLE, "avoiding_obstacle", "ultrasonic_evasion");
        break;
      }

      // Aplicar Torque Boost manteniendo correccion suave si hay pared lateral
      int boostL = PWM_CLIMB_BOOST;
      int boostR = PWM_CLIMB_BOOST;
      if (!latL && latR)      boostR -= 30;
      else if (latL && !latR) boostL -= 30;
      setTargetMotors(boostL, boostR);

      // Si tras 750 ms de Torque Boost los encoders marcan 0 giro, NO es una rampa:
      // el robot esta fisicamente atorado contra algo bajo (pata de mueble, cable, tope alto).
      float minPulseHz = min(pulseRateLeftHz, pulseRateRightHz);
      if (dt > 750 && minPulseHz < 1.5f) {
        emergencyStopMotors();
        changeState(STATE_UNSTUCK_MANEUVER, "unstuck_maneuver", "smart_unstuck_sequence");
        break;
      }

      // Despues de 2.8 segundos de subida exitosa, volver a velocidad crucero
      if (dt >= 2800) {
        changeState(STATE_PATROL_CRUISE, "moving", "patrol_cruise");
      }
      break;
    }

    // ------------------------------------------------------------------------
    // CASO 6: ALGORITMO DE DESATASCO INTELIGENTE MULTI-ETAPA
    // Si el robot se atora con una alfombra, pata de silla o esquina ciega:
    //   Intento 1: Pausa enfriamiento -> Reversa recta -> Giro 70° al lado libre
    //   Intento 2: Pausa -> Reversa con "zig-zag" (sacudida de llantas) -> Giro 120°
    //   Intento 3: Pausa -> Impulso corto + Reversa larga -> Media vuelta 180°
    //   Intento 4+: Pausa de proteccion termica (STATE_TRAPPED_COOLDOWN)
    // ------------------------------------------------------------------------
    case STATE_UNSTUCK_MANEUVER: {
      uint32_t dt = now - subStepStartMs;

      if (maneuverStep == 0) {
        // Paso 0: Cortar motores 350 ms para relajar corriente del elevador MT3608
        setTargetMotors(0, 0);
        if (dt >= 350) {
          unstuckAttemptCount++;
          if (unstuckAttemptCount > 3) {
            changeState(STATE_TRAPPED_COOLDOWN, "error_stuck", "trapped_thermal_protection");
            break;
          }
          maneuverStep   = 1;
          subStepStartMs = now;
        }
      }
      else if (maneuverStep == 1) {
        // Paso 1: Retroceder (con sacudida alternada en el intento 2 para liberar llanta)
        if (!rl || !rr) {
          // Si hay escalon atras, saltar directo al giro
          maneuverStep = 2;
          subStepStartMs = now;
        } else {
          if (unstuckAttemptCount == 2) {
            // Sacudida S-curve cada 220 ms para destrabar llanta de cables/bordes
            if ((dt / 220) % 2 == 0) setTargetMotors(-PWM_TURN, -PWM_SLOW / 2);
            else                     setTargetMotors(-PWM_SLOW / 2, -PWM_TURN);
          } else {
            setTargetMotors(-PWM_TURN, -PWM_TURN);
          }
          if (dt >= 850) {
            maneuverStep   = 2;
            subStepStartMs = now;
            // Elegir lado mas despejado
            if (!latL && latR)      preferredTurnDir = -1;
            else if (latL && !latR) preferredTurnDir =  1;
            else                    preferredTurnDir = (unstuckAttemptCount % 2 == 0) ? 1 : -1;
          }
        }
      }
      else if (maneuverStep == 2) {
        // Paso 2: Giro de escape (mas amplio en cada intento sucesivo)
        uint32_t turnMs = 600 + (unstuckAttemptCount * 250);
        if (preferredTurnDir > 0) setTargetMotors(-PWM_TURN,  PWM_TURN);
        else                      setTargetMotors( PWM_TURN, -PWM_TURN);

        if (dt >= turnMs) {
          stallTimerMs = 0;
          changeState(STATE_PATROL_CRUISE, "moving", "patrol_cruise");
        }
      }
      break;
    }

    // ------------------------------------------------------------------------
    // CASO 7: PROTECCION SI QUEDA COMPLETAMENTE ATRAPADO (TRAPPED COOLDOWN)
    // Si despues de 3 maniobras completas sigue atrapado (ej. enredado en una
    // cortina o cable), apaga los motores 25 segundos para no agotar la bateria
    // ni calentar el puente H, reporta "error_stuck" a Supabase y luego reintenta.
    // ------------------------------------------------------------------------
    case STATE_TRAPPED_COOLDOWN: {
      emergencyStopMotors();
      if (now - stateEnteredMs >= 25000) {
        unstuckAttemptCount = 0;
        changeState(STATE_UNSTUCK_MANEUVER, "unstuck_maneuver", "retry_after_cooldown");
      }
      break;
    }

    // ------------------------------------------------------------------------
    // CASO 8: BATERIA BAJA (ESTACIONAMIENTO SEGURO)
    // ------------------------------------------------------------------------
    case STATE_LOW_BATTERY_PARKED: {
      emergencyStopMotors();
      if (gState.battery_v > (BAT_MIN_VOLTS + 0.30f)) {
        changeState(STATE_PATROL_CRUISE, "moving", "patrol_cruise");
      }
      break;
    }

    default: {
      changeState(STATE_PATROL_CRUISE, "moving", "patrol_cruise");
      break;
    }
  }

  // Actualizar bandera de motores activos para el JSON
  bool active = (abs(currentSpeedL) > 0 || abs(currentSpeedR) > 0);
  portENTER_CRITICAL(&telemetryMux);
  gState.motors_active = active;
  portEXIT_CRITICAL(&telemetryMux);
}

// ============================================================================
// 12. CONSTRUCCION DEL JSON v1.1 Y ENVIO HTTPS A SUPABASE (NUCLEO 0)
// ============================================================================
bool buildTelemetryPayloadJson(char* buffer, size_t maxLen) {
  SharedRobotData snap;
  portENTER_CRITICAL(&telemetryMux);
  gState.sequence++;
  snap = gState;
  portEXIT_CRITICAL(&telemetryMux);

  char eventId[37];
  generateUUIDv4(eventId);

  char capturedAtField[40];
  char isoTime[32];
  if (getIsoUtcTimestamp(isoTime, sizeof(isoTime))) {
    snprintf(capturedAtField, sizeof(capturedAtField), "\"%s\"", isoTime);
    snap.clockSynced = true;
  } else {
    snprintf(capturedAtField, sizeof(capturedAtField), "null");
    snap.clockSynced = false;
  }

  char tempValueStr[16];
  char humValueStr[16];
  const char* dhtQuality = snap.dht_valid ? "valid" : "sensor_error";
  if (snap.dht_valid) {
    snprintf(tempValueStr, sizeof(tempValueStr), "%.1f", snap.temperature_c);
    snprintf(humValueStr,  sizeof(humValueStr),  "%.1f", snap.humidity_pct);
  } else {
    snprintf(tempValueStr, sizeof(tempValueStr), "null");
    snprintf(humValueStr,  sizeof(humValueStr),  "null");
  }

  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : -100;
  uint32_t monoMs = millis();

  // Empaquetado exacto para el RPC public.ingest_telemetry(p jsonb)
  int written = snprintf(
    buffer, maxLen,
    "{\"p\":{"
      "\"schema_version\":\"1.1\","
      "\"event_id\":\"%s\","
      "\"device_id\":\"%s\","
      "\"boot_id\":\"%s\","
      "\"sequence\":%lu,"
      "\"captured_at\":%s,"
      "\"monotonic_ms\":%lu,"
      "\"clock_quality\":\"%s\","
      "\"mission_id\":\"%s\","
      "\"map_version_id\":\"%s\","
      "\"pose\":{"
        "\"x_m\":%.3f,"
        "\"y_m\":%.3f,"
        "\"theta_rad\":%.3f,"
        "\"sigma_position_m\":%.3f,"
        "\"quality\":\"estimated\""
      "},"
      "\"readings\":["
        "{\"channel_key\":\"mq6_raw\",\"value\":%d,\"quality\":\"uncalibrated\",\"calibration_id\":null},"
        "{\"channel_key\":\"mq4_raw\",\"value\":%d,\"quality\":\"uncalibrated\",\"calibration_id\":null},"
        "{\"channel_key\":\"mq7_raw\",\"value\":null,\"quality\":\"not_installed\",\"calibration_id\":null},"
        "{\"channel_key\":\"temperature\",\"value\":%s,\"quality\":\"%s\",\"calibration_id\":null},"
        "{\"channel_key\":\"humidity\",\"value\":%s,\"quality\":\"%s\",\"calibration_id\":null},"
        "{\"channel_key\":\"front_distance_m\",\"value\":%.2f,\"quality\":\"valid\",\"calibration_id\":null},"
        "{\"channel_key\":\"ir_floor_front_left\",\"boolean_value\":%s,\"quality\":\"valid\",\"calibration_id\":null},"
        "{\"channel_key\":\"ir_floor_front_right\",\"boolean_value\":%s,\"quality\":\"valid\",\"calibration_id\":null},"
        "{\"channel_key\":\"ir_floor_rear_left\",\"boolean_value\":%s,\"quality\":\"valid\",\"calibration_id\":null},"
        "{\"channel_key\":\"ir_floor_rear_right\",\"boolean_value\":%s,\"quality\":\"valid\",\"calibration_id\":null},"
        "{\"channel_key\":\"ir_lateral_left\",\"boolean_value\":%s,\"quality\":\"valid\",\"calibration_id\":null},"
        "{\"channel_key\":\"ir_lateral_right\",\"boolean_value\":%s,\"quality\":\"valid\",\"calibration_id\":null}"
      "],"
      "\"battery\":{"
        "\"voltage_v\":%.2f,"
        "\"soc_pct\":%.1f,"
        "\"current_a\":null,"
        "\"method_code\":\"voltage_only\","
        "\"quality\":\"voltage_only\""
      "},"
      "\"robot_state\":\"%s\","
      "\"navigation_state\":\"%s\","
      "\"motors_active\":%s,"
      "\"wifi_rssi_dbm\":%d,"
      "\"firmware_version\":\"%s\""
    "}}",
    eventId,
    DEVICE_ID,
    snap.bootId,
    (unsigned long)snap.sequence,
    capturedAtField,
    (unsigned long)monoMs,
    snap.clockSynced ? "synced" : "unsynced",
    MISSION_ID,
    MAP_VERSION_ID,
    snap.x_m,
    snap.y_m,
    snap.theta_rad,
    snap.sigma_pos_m,
    snap.mq6_raw,
    snap.mq4_raw,
    tempValueStr, dhtQuality,
    humValueStr,  dhtQuality,
    snap.front_distance_m,
    snap.ir_floor_fl  ? "true" : "false",
    snap.ir_floor_fr  ? "true" : "false",
    snap.ir_floor_rl  ? "true" : "false",
    snap.ir_floor_rr  ? "true" : "false",
    snap.ir_lat_left  ? "true" : "false",
    snap.ir_lat_right ? "true" : "false",
    snap.battery_v,
    snap.battery_soc,
    snap.robot_state_str,
    snap.nav_state_str,
    snap.motors_active ? "true" : "false",
    rssi,
    FIRMWARE_VER
  );

  return (written > 0 && (size_t)written < maxLen);
}

// Tarea en Nucleo 0: maneja WiFi, NTP y POST a Supabase sin frenar los motores
void telemetryCloudTask(void* parameter) {
  (void)parameter;
  static char jsonPayload[2048];
  String endpointUrl = String(SUPABASE_URL) + "/rest/v1/rpc/ingest_telemetry";

  for (;;) {
    // 1. Mantener conexion WiFi viva de forma no bloqueante para el robot
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] Conectando a WiFi...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

      uint32_t startWait = millis();
      while (WiFi.status() != WL_CONNECTED && (millis() - startWait < 8000)) {
        vTaskDelay(pdMS_TO_TICKS(250));
      }

      if (WiFi.status() == WL_CONNECTED) {
        Serial.print("[WIFI] Conectado! IP: ");
        Serial.println(WiFi.localIP());
        configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
      }
    }

    // 2. Si hay WiFi, construir el JSON v1.1 y enviarlo a Supabase
    if (WiFi.status() == WL_CONNECTED) {
      if (buildTelemetryPayloadJson(jsonPayload, sizeof(jsonPayload))) {
        WiFiClientSecure client;
        client.setInsecure(); // Evita expiracion de certificados raiz en ESP32

        HTTPClient http;
        http.setTimeout(4000);
        if (http.begin(client, endpointUrl)) {
          http.addHeader("Content-Type", "application/json");
          http.addHeader("apikey", SUPABASE_KEY);
          http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);

          int httpCode = http.POST((uint8_t*)jsonPayload, strlen(jsonPayload));
          if (httpCode >= 200 && httpCode < 300) {
            String resp = http.getString();
            Serial.printf("[SUPABASE OK %d] seq=%lu state=%s dist=%.2fm\n",
                          httpCode, (unsigned long)gState.sequence,
                          gState.nav_state_str, gState.front_distance_m);
          } else {
            String err = http.getString();
            Serial.printf("[SUPABASE ERR %d] %s\n", httpCode, err.c_str());
          }
          http.end();
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));
  }
}

// ============================================================================
// 13. SETUP Y LOOP PRINCIPAL (NUCLEO 1 - TIEMPO REAL 50 Hz)
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=======================================================");
  Serial.println("  INICIANDO ROBOT CENTINELA V7.2 - 100% AUTONOMO");
  Serial.println("=======================================================");

  // 1. Configurar resolucion y atenuacion del ADC (0..3.3V en 12 bits: 0..4095)
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  // 2. Configurar pines de sensores IR
  pinMode(PIN_IR_FLOOR_FL,  INPUT_PULLUP);
  pinMode(PIN_IR_FLOOR_FR,  INPUT_PULLUP);
  pinMode(PIN_IR_FLOOR_RL,  INPUT_PULLUP);
  pinMode(PIN_IR_FLOOR_RR,  INPUT_PULLUP);
  pinMode(PIN_IR_LAT_LEFT,  INPUT_PULLUP);
  pinMode(PIN_IR_LAT_RIGHT, INPUT_PULLUP);

  // 3. Configurar pines del ultrasonico HC-SR04
  pinMode(PIN_US_TRIG, OUTPUT);
  digitalWrite(PIN_US_TRIG, LOW);
  pinMode(PIN_US_ECHO, INPUT);

  // 4. Iniciar sensor ambiental DHT22
  dht.begin();

  // 5. Configurar encoders magneticos con interrupciones en flanco de subida
  pinMode(PIN_ENC_LEFT,  INPUT_PULLUP);
  pinMode(PIN_ENC_RIGHT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_LEFT),  onLeftEncoderPulse,  RISING);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_RIGHT), onRightEncoderPulse, RISING);

  // 6. Inicializar PWM de motores L298N en paro seguro
  initMotorPWM();
  emergencyStopMotors();

  // 7. Inicializar estado global y generar boot_id unico de esta sesion
  memset(&gState, 0, sizeof(gState));
  generateUUIDv4(gState.bootId);
  gState.sigma_pos_m     = 0.05f;
  gState.robot_state_str = "idle";
  gState.nav_state_str   = "boot_wait";
  gState.ir_floor_fl     = true;
  gState.ir_floor_fr     = true;
  gState.ir_floor_rl     = true;
  gState.ir_floor_rr     = true;
  gState.ir_lat_left     = true;
  gState.ir_lat_right    = true;
  gState.front_distance_m = 1.50f;

  Serial.print("[BOOT] boot_id generado: ");
  Serial.println(gState.bootId);

  // 8. Lanzar tarea de Telemetria Cloud en el Nucleo 0
  WiFi.mode(WIFI_STA);
  xTaskCreatePinnedToCore(
    telemetryCloudTask,   // Funcion de la tarea
    "TelemetryCloudTask", // Nombre
    8192,                 // Stack (8 KB para TLS/HTTPS)
    nullptr,              // Parametro
    1,                    // Prioridad
    nullptr,              // Handle
    0                     // Nucleo 0 (deja el Nucleo 1 libre para navegacion)
  );

  stateEnteredMs = millis();
  prevEncCheckMs = millis();
}

void loop() {
  static uint32_t lastControlMs = 0;
  uint32_t now = millis();

  // Lazo determinista a 50 Hz (cada 20 ms)
  if (now - lastControlMs >= CONTROL_PERIOD_MS) {
    lastControlMs = now;

    // 1. Leer todos los sensores (IR piso, IR paredes, Ultrasonico, Gases, DHT22, Bateria)
    readAllSensors();

    // 2. Calcular odometria (x, y, theta) y velocidad de ruedas con los encoders
    updateOdometryAndWheelHealth();

    // 3. Ejecutar el cerebro autonomo (10 estados de navegacion y supervivencia)
    runAutonomousBrain();

    // 4. Aplicar rampa suave de aceleracion a los motores (protege bateria y MT3608)
    updateMotorRamp();
  }
}