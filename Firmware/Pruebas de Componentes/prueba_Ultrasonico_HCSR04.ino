/*
 * ============================================================
 *  PRUEBA INDIVIDUAL DEL SENSOR ULTRASÓNICO HC-SR04
 *  Robot Centinela V7.2 - Banco de pruebas (circuito independiente)
 * ============================================================
 *
 *  Todo va del lado IZQUIERDO del ESP32 (lado de VN y VIN/5V):
 *
 *     HC-SR04         ESP32 (NodeMCU-32S)
 *     -------         -------------------
 *     VCC   ------->  VIN (5V del USB, último pin abajo a la izquierda)
 *     GND   ------->  GND (pin entre D12 y D13)
 *     TRIG  ------->  D32 (GPIO 32), directo
 *     ECHO  --[1kΩ]--+-->  VP (GPIO 36), justo debajo de EN
 *                    |
 *                  [2kΩ]
 *                    |
 *                   GND
 *
 *  ECHO va en VP (GPIO 36), el pin más cercano a EN, porque es el que
 *  lleva el divisor de resistencias. GPIO 36 es solo de entrada, ideal
 *  para leer el eco. TRIG necesita un pin de salida, por eso va en D32
 *  (VP, VN, 34 y 35 no pueden ser salidas).
 *
 *  El divisor 1k/2k es OBLIGATORIO: ECHO sale a 5V y lo baja a ~3.3V.
 *  Si no tienes de 2kΩ, usa dos de 1kΩ en serie.
 *
 *  INDICADORES:
 *     - Monitor Serie a 115200 baudios: distancia en cm y una barra.
 *     - LED azul integrado del ESP32 (GPIO 2, no requiere cables)
 *       se enciende si hay algo a menos de DISTANCIA_ALERTA_CM.
 * ============================================================
 */

// ---------------- CONFIGURACIÓN ----------------
const int PIN_TRIG = 32;   // D32
const int PIN_ECHO = 36;   // VP
const int PIN_LED  = 2;    // LED integrado

const float DISTANCIA_ALERTA_CM = 20.0;   // Umbral para encender el LED
const int   MUESTRAS            = 5;      // Lecturas por medición (se usa la mediana)
const unsigned long TIMEOUT_US  = 30000;  // ~5 m; si no hay eco, "fuera de rango"
const unsigned long PERIODO_MS  = 200;    // Cada cuánto medir
// ------------------------------------------------

// Un disparo: devuelve distancia en cm (-1 si no hubo eco)
float medirUnaVez() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(3);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);                  // Pulso de disparo de 10 µs
  digitalWrite(PIN_TRIG, LOW);

  unsigned long duracion = pulseIn(PIN_ECHO, HIGH, TIMEOUT_US);
  if (duracion == 0) return -1;

  // Sonido ~343 m/s = 0.0343 cm/µs; ida y vuelta -> /2
  return duracion * 0.0343 / 2.0;
}

// Varias lecturas -> mediana (descarta lecturas raras)
float medirDistancia() {
  float lecturas[MUESTRAS];
  int validas = 0;

  for (int i = 0; i < MUESTRAS; i++) {
    float d = medirUnaVez();
    if (d > 0) lecturas[validas++] = d;
    delay(15);                            // Deja apagar los ecos anteriores
  }
  if (validas == 0) return -1;

  for (int i = 0; i < validas - 1; i++)
    for (int j = i + 1; j < validas; j++)
      if (lecturas[j] < lecturas[i]) {
        float t = lecturas[i]; lecturas[i] = lecturas[j]; lecturas[j] = t;
      }

  return lecturas[validas / 2];
}

void imprimirBarra(float cm) {
  int bloques = constrain((int)(cm / 5.0), 0, 40);   // 1 bloque = 5 cm
  Serial.print(" |");
  for (int i = 0; i < bloques; i++) Serial.print("#");
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);               // GPIO 36 no tiene pull-up interno (no lo necesita)
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_TRIG, LOW);

  Serial.println();
  Serial.println("==============================================");
  Serial.println("      PRUEBA DE SENSOR ULTRASONICO HC-SR04");
  Serial.println("==============================================");
  Serial.println("TRIG -> D32   |   ECHO (con divisor) -> VP (GPIO 36)");
  Serial.println("Rango util: ~2 cm a ~400 cm");
  Serial.println("Pon la mano o un libro frente al sensor y muevelo.");
  Serial.println("----------------------------------------------");
}

void loop() {
  static unsigned long ultimo = 0;
  static int fallosSeguidos = 0;

  if (millis() - ultimo < PERIODO_MS) return;
  ultimo = millis();

  float cm = medirDistancia();

  if (cm < 0) {
    fallosSeguidos++;
    digitalWrite(PIN_LED, LOW);
    Serial.println("Fuera de rango / sin eco");

    if (fallosSeguidos == 10) {
      Serial.println("!! 10 lecturas sin eco seguidas. Revisa:");
      Serial.println("   - VCC del sensor en VIN (5V), no en 3V3");
      Serial.println("   - TRIG en D32");
      Serial.println("   - ECHO -> 1k -> VP, y del punto medio 2k a GND");
      Serial.println("   - GND del sensor al GND del ESP32");
      Serial.println("   - Que haya algo frente al sensor (< 4 m)");
    }
    return;
  }

  fallosSeguidos = 0;
  digitalWrite(PIN_LED, cm < DISTANCIA_ALERTA_CM ? HIGH : LOW);

  Serial.print("Distancia: ");
  if (cm < 100) Serial.print(" ");
  if (cm < 10)  Serial.print(" ");
  Serial.print(cm, 1);
  Serial.print(" cm");
  if (cm < DISTANCIA_ALERTA_CM) Serial.print("  <<< CERCA");
  imprimirBarra(cm);
}
