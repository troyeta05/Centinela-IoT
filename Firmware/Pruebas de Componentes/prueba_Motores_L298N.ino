/*
 * ============================================================
 *  PRUEBA DE MOTORES GM25-370 (12V, 140 RPM, con encoder) + L298N
 *  Robot Centinela V7.2 - Banco de pruebas  (v2: todo del lado derecho)
 * ============================================================
 *
 *  CONEXIONES ESP32 -> L298N:
 *
 *     ESP32  D4  (GPIO 4)  ---> IN1   \  Motor A (salidas OUT1 / OUT2)
 *     ESP32  D16 (GPIO 16) ---> IN2   /
 *     ESP32  D17 (GPIO 17) ---> IN3   \  Motor B (salidas OUT3 / OUT4)
 *     ESP32  D18  (GPIO 18)  ---> IN4 /
 *     ESP32  GND ------------> GND del L298N   <-- OBLIGATORIO (tierra común)
 *
 *  L298N:
 *     12V  <--- (+) pila 9V
 *     GND  <--- (-) pila 9V  (y también al GND del ESP32)
 *     Jumpers ENA y ENB PUESTOS (la velocidad se controla con PWM en los IN)
 *     Jumper de 5V PUESTO
 *
 *  MOTORES:
 *     Pin 1 motor A -> OUT1   | Pin 2 motor A -> OUT2
 *     Pin 2 motor B -> OUT3   | Pin 1 motor B -> OUT4   (cableado en espejo)
 *     Pin 3 (ambos) -> Bus GND
 *     Pin 4 (ambos) -> Bus 3.3V
 *     Pin 5 de un motor  -> D21 (GPIO 21)  encoder canal A
 *     Pin 5 del otro     -> D23 (GPIO 23)  encoder canal A
 *     Pin 6 (canal B)    -> sin conectar
 *
 *  AVISO GPIO 5 (IN4): es un pin de arranque del ESP32 y se queda en ALTO
 *  mientras el chip arranca o se está subiendo un programa. Eso puede hacer
 *  que el motor B gire solo durante la carga. Desconecta la pila al subir
 *  el código, o mueve IN4 a D18 / D19 y cambia la constante IN4 abajo.
 *
 *  Si alguna rueda gira al revés, cambia INVERTIR_A o INVERTIR_B a true.
 *
 *  USO:
 *   - Monitor Serie a 115200 baudios.
 *   - Al arrancar corre una SECUENCIA AUTOMÁTICA que además te dice
 *     qué encoder (D21 o D23) pertenece a qué motor.
 *   - Después puedes controlar a mano con el menú (escribe 'h').
 * ============================================================
 */

#include <Arduino.h>

// ---------------- PINES ----------------
const int IN1 = 4;
const int IN2 = 16;
const int IN3 = 17;
const int IN4 = 18;     

const int ENC_D21 = 21;
const int ENC_D23 = 23;

// ---------------- AJUSTES ----------------
bool INVERTIR_A = false;          // Cambia a true si el motor A gira al revés
bool INVERTIR_B = false;          // Cambia a true si el motor B gira al revés

int velocidad = 200;              // 0-255. Con pila de 9V, debajo de ~90 quizá no arranquen
const int PASO_VEL   = 25;
const int VEL_MINIMA = 60;

const int PWM_FREQ = 1000;        // 1 kHz: el L298N trabaja bien a baja frecuencia
const int PWM_RES  = 8;           // 8 bits -> 0..255

const unsigned long PERIODO_IMPRESION = 500;   // ms entre reportes de encoders

// ---------------- ENCODERS ----------------
volatile long cuentaD21 = 0;
volatile long cuentaD23 = 0;
void IRAM_ATTR isrD21() { cuentaD21++; }
void IRAM_ATTR isrD23() { cuentaD23++; }

float pulsosPorVueltaD21 = 0;     // Se llenan con la calibración ('c')
float pulsosPorVueltaD23 = 0;
bool  calibrando = false;

// ---------------- ESTADO ----------------
int dirA = 0, dirB = 0;           // -1 atrás, 0 parado, 1 adelante
int velActualA = 0, velActualB = 0;

// ============================================================
//  PWM compatible con núcleo ESP32 2.x y 3.x
// ============================================================
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  void pwmIniciar(int pin, int canal) { ledcAttach(pin, PWM_FREQ, PWM_RES); }
  void pwmEscribir(int pin, int canal, int duty) { ledcWrite(pin, duty); }
#else
  void pwmIniciar(int pin, int canal) { ledcSetup(canal, PWM_FREQ, PWM_RES); ledcAttachPin(pin, canal); }
  void pwmEscribir(int pin, int canal, int duty) { ledcWrite(canal, duty); }
#endif

// Canales PWM (solo los usa el núcleo 2.x)
const int CH_IN1 = 0, CH_IN2 = 1, CH_IN3 = 2, CH_IN4 = 3;

// ============================================================
//  CONTROL DE MOTORES
// ============================================================
// v de -255 (atrás) a 255 (adelante). 0 = rueda libre.
void motorA(int v) {
  if (INVERTIR_A) v = -v;
  v = constrain(v, -255, 255);
  if (v > 0)      { pwmEscribir(IN1, CH_IN1, v);  pwmEscribir(IN2, CH_IN2, 0); }
  else if (v < 0) { pwmEscribir(IN1, CH_IN1, 0);  pwmEscribir(IN2, CH_IN2, -v); }
  else            { pwmEscribir(IN1, CH_IN1, 0);  pwmEscribir(IN2, CH_IN2, 0); }
}

void motorB(int v) {
  if (INVERTIR_B) v = -v;
  v = constrain(v, -255, 255);
  if (v > 0)      { pwmEscribir(IN3, CH_IN3, v);  pwmEscribir(IN4, CH_IN4, 0); }
  else if (v < 0) { pwmEscribir(IN3, CH_IN3, 0);  pwmEscribir(IN4, CH_IN4, -v); }
  else            { pwmEscribir(IN3, CH_IN3, 0);  pwmEscribir(IN4, CH_IN4, 0); }
}

// Rampa suave: evita picos de corriente que tumban la pila de 9V
void rampa(int destinoA, int destinoB, int duracionMs = 400) {
  const int pasos = 20;
  int inicioA = velActualA, inicioB = velActualB;
  for (int i = 1; i <= pasos; i++) {
    motorA(inicioA + (destinoA - inicioA) * i / pasos);
    motorB(inicioB + (destinoB - inicioB) * i / pasos);
    delay(duracionMs / pasos);
  }
  velActualA = destinoA;
  velActualB = destinoB;
}

void aplicarDirecciones() {
  rampa(dirA * velocidad, dirB * velocidad);
}

void detener() {
  dirA = dirB = 0;
  rampa(0, 0, 250);
}

// ============================================================
//  REPORTE DE ENCODERS
// ============================================================
void imprimirEncoders() {
  static unsigned long tPrevio = 0;
  static long previoD21 = 0, previoD23 = 0;

  unsigned long ahora = millis();
  if (ahora - tPrevio < PERIODO_IMPRESION) return;

  float dt = (ahora - tPrevio) / 1000.0;
  long c21 = cuentaD21, c23 = cuentaD23;
  float pps21 = (c21 - previoD21) / dt;
  float pps23 = (c23 - previoD23) / dt;
  tPrevio = ahora; previoD21 = c21; previoD23 = c23;

  Serial.printf("  A:%4d  B:%4d | D21: %7ld pulsos (%6.0f p/s", velActualA, velActualB, c21, pps21);
  if (pulsosPorVueltaD21 > 0) Serial.printf(", %5.1f RPM", pps21 * 60.0 / pulsosPorVueltaD21);
  Serial.printf(") | D23: %7ld pulsos (%6.0f p/s", c23, pps23);
  if (pulsosPorVueltaD23 > 0) Serial.printf(", %5.1f RPM", pps23 * 60.0 / pulsosPorVueltaD23);
  Serial.println(")");
}

// Espera ms mientras sigue reportando
void esperar(unsigned long ms) {
  unsigned long t0 = millis();
  while (millis() - t0 < ms) { imprimirEncoders(); delay(5); }
}

// ============================================================
//  SECUENCIA AUTOMÁTICA
// ============================================================
void pasoSecuencia(const char* nombre, int va, int vb, unsigned long ms, long &d21Out, long &d23Out) {
  Serial.printf("\n>>> %s\n", nombre);
  long ini21 = cuentaD21, ini23 = cuentaD23;
  rampa(va, vb);
  esperar(ms);
  rampa(0, 0, 250);
  esperar(600);                     // deja que la rueda se detenga
  d21Out = cuentaD21 - ini21;
  d23Out = cuentaD23 - ini23;
  Serial.printf("    Pulsos en este paso -> D21: %ld | D23: %ld\n", d21Out, d23Out);
}

void identificarEncoder(const char* motor, long d21, long d23) {
  if (d21 < 10 && d23 < 10) {
    Serial.printf("    !! Ningun encoder conto con el motor %s. ¿Giro la rueda?\n", motor);
    Serial.println("       Si no giro: revisa IN del L298N, pila y GND comun.");
    Serial.println("       Si giro: revisa pin 4 a 3.3V, pin 3 a GND y pin 5 al ESP32.");
  } else if (d21 > d23 * 3) {
    Serial.printf("    => El encoder del motor %s esta en D21\n", motor);
  } else if (d23 > d21 * 3) {
    Serial.printf("    => El encoder del motor %s esta en D23\n", motor);
  } else {
    Serial.printf("    ?? Ambos encoders contaron con el motor %s. Revisa cables cruzados.\n", motor);
  }
}

void secuenciaAutomatica() {
  Serial.println("\n==============================================");
  Serial.println("  SECUENCIA AUTOMATICA (pon el robot en alto,");
  Serial.println("  con las ruedas sin tocar la mesa)");
  Serial.println("==============================================");
  long d21, d23;
  int v = velocidad;

  pasoSecuencia("1. Motor A ADELANTE  (IN1=D4 activo)",   v, 0, 2000, d21, d23); identificarEncoder("A", d21, d23);
  pasoSecuencia("2. Motor A ATRAS     (IN2=D16 activo)", -v, 0, 2000, d21, d23);
  pasoSecuencia("3. Motor B ADELANTE  (IN3=D17 activo)",  0, v, 2000, d21, d23); identificarEncoder("B", d21, d23);
  pasoSecuencia("4. Motor B ATRAS     (IN4=D5 activo)",   0,-v, 2000, d21, d23);
  pasoSecuencia("5. AMBOS ADELANTE",    v, v, 2500, d21, d23);
  pasoSecuencia("6. AMBOS ATRAS",      -v,-v, 2500, d21, d23);
  pasoSecuencia("7. GIRO IZQUIERDA",   -v, v, 2000, d21, d23);
  pasoSecuencia("8. GIRO DERECHA",      v,-v, 2000, d21, d23);

  dirA = dirB = 0;
  Serial.println("\n=== Secuencia terminada. Escribe 'h' para ver el menu ===");
}

// ============================================================
//  MENÚ
// ============================================================
void menu() {
  Serial.println("\n------------------- MENU -------------------");
  Serial.println(" 1 = Motor A adelante     2 = Motor A atras");
  Serial.println(" 3 = Motor B adelante     4 = Motor B atras");
  Serial.println(" w = Ambos adelante       x = Ambos atras");
  Serial.println(" a = Giro izquierda       d = Giro derecha");
  Serial.println(" s = DETENER");
  Serial.println(" + = Mas velocidad        - = Menos velocidad");
  Serial.println(" t = Repetir secuencia automatica");
  Serial.println(" c = Calibrar encoders (girar rueda a mano 1 vuelta)");
  Serial.println(" r = Reiniciar contadores de encoders");
  Serial.println(" h = Ver este menu");
  Serial.printf (" Velocidad actual: %d / 255\n", velocidad);
  Serial.println("--------------------------------------------");
}

void procesarComando(char c) {
  if (calibrando && c != 'c' && c != '\n' && c != '\r') {
    Serial.println("Estas calibrando: gira la rueda 1 vuelta y escribe 'c' otra vez.");
    return;
  }

  switch (c) {
    case '1': dirA =  1; dirB = 0; Serial.println("Motor A adelante"); aplicarDirecciones(); break;
    case '2': dirA = -1; dirB = 0; Serial.println("Motor A atras");    aplicarDirecciones(); break;
    case '3': dirA = 0; dirB =  1; Serial.println("Motor B adelante"); aplicarDirecciones(); break;
    case '4': dirA = 0; dirB = -1; Serial.println("Motor B atras");    aplicarDirecciones(); break;
    case 'w': dirA =  1; dirB =  1; Serial.println("Ambos adelante");  aplicarDirecciones(); break;
    case 'x': dirA = -1; dirB = -1; Serial.println("Ambos atras");     aplicarDirecciones(); break;
    case 'a': dirA = -1; dirB =  1; Serial.println("Giro izquierda");  aplicarDirecciones(); break;
    case 'd': dirA =  1; dirB = -1; Serial.println("Giro derecha");    aplicarDirecciones(); break;
    case 's': Serial.println("DETENIDO"); detener(); break;

    case '+':
      velocidad = min(255, velocidad + PASO_VEL);
      Serial.printf("Velocidad: %d\n", velocidad); aplicarDirecciones(); break;
    case '-':
      velocidad = max(VEL_MINIMA, velocidad - PASO_VEL);
      Serial.printf("Velocidad: %d\n", velocidad); aplicarDirecciones(); break;

    case 't': detener(); secuenciaAutomatica(); break;

    case 'r':
      cuentaD21 = cuentaD23 = 0;
      Serial.println("Contadores en cero."); break;

    case 'c':
      if (!calibrando) {
        detener();
        cuentaD21 = cuentaD23 = 0;
        calibrando = true;
        Serial.println("\nCALIBRACION: marca la rueda con cinta, girala a mano");
        Serial.println("exactamente 1 vuelta (en el mismo sentido) y escribe 'c'.");
      } else {
        calibrando = false;
        long c21 = cuentaD21, c23 = cuentaD23;
        if (c21 > 0) pulsosPorVueltaD21 = c21;
        if (c23 > 0) pulsosPorVueltaD23 = c23;
        Serial.printf("Pulsos por vuelta -> D21: %ld | D23: %ld\n", c21, c23);
        Serial.println("(Solo cuenta la rueda que giraste; calibra la otra igual.)");
        Serial.println("Desde ahora el reporte tambien muestra RPM.");
      }
      break;

    case 'h': menu(); break;
    case '\n': case '\r': case ' ': break;
    default: Serial.printf("Comando '%c' no reconocido. Escribe 'h'.\n", c);
  }
}

// ============================================================
void setup() {
  // Lo PRIMERO: apagar los IN para que ningún motor se quede girando
  // por el estado en que dejó los pines el arranque (sobre todo GPIO 5).
  pinMode(IN1, OUTPUT); digitalWrite(IN1, LOW);
  pinMode(IN2, OUTPUT); digitalWrite(IN2, LOW);
  pinMode(IN3, OUTPUT); digitalWrite(IN3, LOW);
  pinMode(IN4, OUTPUT); digitalWrite(IN4, LOW);

  Serial.begin(115200);
  delay(500);

  pwmIniciar(IN1, CH_IN1);
  pwmIniciar(IN2, CH_IN2);
  pwmIniciar(IN3, CH_IN3);
  pwmIniciar(IN4, CH_IN4);
  motorA(0); motorB(0);

  pinMode(ENC_D21, INPUT_PULLUP);
  pinMode(ENC_D23, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_D21), isrD21, RISING);
  attachInterrupt(digitalPinToInterrupt(ENC_D23), isrD23, RISING);

  Serial.println("\n==============================================");
  Serial.println("   PRUEBA MOTORES GM25-370 + L298N + ENCODERS");
  Serial.println("==============================================");
  Serial.println("IN1=D4  IN2=D16  IN3=D17  IN4=D5 | Encoders: D21 y D23");
  Serial.println("La secuencia automatica empieza en 3 segundos...");
  delay(3000);

  secuenciaAutomatica();
  menu();
}

void loop() {
  while (Serial.available()) procesarComando(tolower(Serial.read()));
  imprimirEncoders();
}