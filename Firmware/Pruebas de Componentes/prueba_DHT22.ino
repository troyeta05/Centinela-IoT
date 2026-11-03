/*
 * ============================================================
 *  PRUEBA INDIVIDUAL DEL SENSOR DHT22 (Temperatura y Humedad)
 *  Robot Centinela V7.2 - Banco de pruebas (circuito independiente)
 * ============================================================
 *
 *  El DHT22 funciona con 3.3V, así que todo va cerca del pin 3V3
 *  (parte de arriba del ESP32):
 *
 *     DHT22 (módulo)    ESP32 (NodeMCU-32S)
 *     --------------    -------------------
 *     VCC (+)  ------>  3V3  (arriba a la izquierda)
 *     DAT (OUT) ----->  D15 (GPIO 15)
 *     GND (-)  ------>  GND  (arriba a la derecha, frente al 3V3)
 *
 *  D32 es el pin de salida/entrada más cercano al 3V3. (VP, VN, 34 y 35
 *  están más cerca, pero solo leen, y el DHT22 necesita un pin que
 *  pueda enviar y recibir.)
 *
 *  Si tu DHT22 es el sensor SUELTO de 4 patitas (sin placa):
 *     Pata 1 -> 3V3 | Pata 2 -> D32 | Pata 3 -> nada | Pata 4 -> GND
 *     y pon una resistencia de 10kΩ entre la pata 2 y 3V3.
 *  El módulo de 3 pines ya trae esa resistencia.
 *
 *  LIBRERÍAS (Programa -> Incluir Librería -> Administrar Bibliotecas):
 *     - "DHT sensor library" de Adafruit
 *     - "Adafruit Unified Sensor" (te la pide al instalar la anterior)
 *
 *  INDICADORES:
 *     - Monitor Serie a 115200 baudios.
 *     - LED azul integrado (GPIO 2): parpadea con cada lectura correcta.
 * ============================================================
 */

#include <DHT.h>

// ---------------- CONFIGURACIÓN ----------------
const int PIN_DHT = 15;            // D15
const int PIN_LED = 2;             // LED integrado
#define TIPO_DHT DHT22

const unsigned long PERIODO_MS = 2500;   // El DHT22 necesita mínimo 2 s entre lecturas
// ------------------------------------------------

DHT dht(PIN_DHT, TIPO_DHT);

unsigned long lecturasOK = 0;
unsigned long lecturasError = 0;
int erroresSeguidos = 0;

float tMin = 1000, tMax = -1000;
float hMin = 1000, hMax = -1000;

// Punto de rocío (aproximación de Magnus)
float puntoRocio(float t, float h) {
  const float a = 17.27, b = 237.7;
  float alfa = (a * t) / (b + t) + log(h / 100.0);
  return (b * alfa) / (a - alfa);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(PIN_LED, OUTPUT);

  dht.begin();

  Serial.println();
  Serial.println("==============================================");
  Serial.println("        PRUEBA DE SENSOR DHT22 (TEMP/HUM)");
  Serial.println("==============================================");
  Serial.println("VCC -> 3V3  |  DAT -> D32  |  GND -> GND");
  Serial.println("Lectura cada 2.5 s. Para probar: sopla sobre el");
  Serial.println("sensor (sube la humedad) o tocalo con los dedos");
  Serial.println("(sube la temperatura poco a poco).");
  Serial.println("----------------------------------------------");
  delay(2000);                     // El sensor necesita ~2 s al encender
}

void loop() {
  static unsigned long ultimo = 0;
  if (millis() - ultimo < PERIODO_MS) return;
  ultimo = millis();

  float h = dht.readHumidity();
  float t = dht.readTemperature();         // °C

  if (isnan(h) || isnan(t)) {
    lecturasError++;
    erroresSeguidos++;
    Serial.printf("[ERROR] No se pudo leer el DHT22 (errores seguidos: %d)\n", erroresSeguidos);

    if (erroresSeguidos == 5) {
      Serial.println("!! 5 errores seguidos. Revisa:");
      Serial.println("   - DAT conectado en D32");
      Serial.println("   - VCC en 3V3 y GND en GND");
      Serial.println("   - Si es el sensor suelto de 4 patas: resistencia 10k entre DAT y 3V3");
      Serial.println("   - Que la libreria sea 'DHT sensor library' de Adafruit");
      Serial.println("   - Que el codigo diga DHT22 y no DHT11");
    }
    return;
  }

  erroresSeguidos = 0;
  lecturasOK++;

  tMin = min(tMin, t); tMax = max(tMax, t);
  hMin = min(hMin, h); hMax = max(hMax, h);

  float st = dht.computeHeatIndex(t, h, false);   // Sensación térmica en °C
  float pr = puntoRocio(t, h);

  // Parpadeo de confirmación
  digitalWrite(PIN_LED, HIGH);

  Serial.printf("Temp: %5.1f °C | Humedad: %5.1f %% | Sensacion: %5.1f °C | Rocio: %5.1f °C\n",
                t, h, st, pr);

  // Resumen cada 10 lecturas correctas
  if (lecturasOK % 10 == 0) {
    float exito = 100.0 * lecturasOK / (lecturasOK + lecturasError);
    Serial.println("   ------------------------------------------");
    Serial.printf ("   Temp min/max: %.1f / %.1f °C\n", tMin, tMax);
    Serial.printf ("   Hum  min/max: %.1f / %.1f %%\n", hMin, hMax);
    Serial.printf ("   Lecturas OK: %lu | Errores: %lu | Exito: %.0f %%\n",
                   lecturasOK, lecturasError, exito);
    Serial.println("   ------------------------------------------");
  }

  delay(80);
  digitalWrite(PIN_LED, LOW);
}