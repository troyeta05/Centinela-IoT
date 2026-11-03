/*
 * ============================================================
 *  PRUEBA INDIVIDUAL DE SENSORES INFRARROJOS TCRT5000
 *  Robot Centinela V7.2 - Banco de pruebas (1 sensor a la vez)
 * ============================================================
 *
 *  CONEXIÓN (igual para los 6 sensores, se cambia uno por uno):
 *
 *     TCRT5000        ESP32 (NodeMCU-32S)
 *     --------        -------------------
 *     VCC   ------->  3V3   (pin de arriba a la izquierda)
 *     GND   ------->  GND   (el pin justo arriba de D13)
 *     D0    ------->  D15   (GPIO 15, al lado del GND)
 *
 *  Se usa GPIO 13 porque queda pegado a un GND en la placa:
 *  conectas GND y D0 en dos pines contiguos y no te equivocas.
 *
 *  IMPORTANTE: para la prueba alimenta el sensor con 3.3V, no con 5V.
 *  Con 5V la salida D0 sube a 5V y puede dañar la entrada del ESP32.
 *
 *  LÓGICA DEL TCRT5000 (módulo con comparador LM393):
 *     D0 = LOW  -> Hay superficie/objeto reflejando (LED del módulo encendido)
 *     D0 = HIGH -> No hay reflejo (vacío, escalón, superficie negra o lejos)
 *
 *  INDICADORES:
 *     - LED azul integrado del ESP32 (GPIO 2) se enciende al DETECTAR.
 *     - Monitor Serie a 115200 baudios muestra cada cambio y un resumen.
 *
 *  Ajusta la distancia de detección con el potenciómetro azul del módulo.
 * ============================================================
 */

// ---------------- CONFIGURACIÓN ----------------
const int PIN_SENSOR = 15;   // D0 del TCRT5000
const int PIN_LED    = 2;    // LED integrado del ESP32

// Cambia este texto cada vez que conectes otro sensor (solo es una etiqueta)
String nombreSensor = "PISO FRONT IZQ";
// Opciones: "PISO FRONT IZQ", "PISO FRONT DER", "PISO TRAS IZQ",
//           "PISO TRAS DER", "PARED LAT IZQ", "PARED LAT DER"

const unsigned long ANTIRREBOTE_MS = 20;     // Filtra parpadeos de la señal
const unsigned long RESUMEN_MS     = 2000;   // Cada cuánto imprimir el estado
// ------------------------------------------------

int estadoEstable      = HIGH;
int ultimaLectura      = HIGH;
unsigned long tCambio  = 0;
unsigned long tResumen = 0;
unsigned long detecciones = 0;
unsigned long tInicioDeteccion = 0;

void imprimirEstado(int estado) {
  if (estado == LOW) {
    Serial.println(">>> DETECTA superficie / objeto  (D0 = LOW)");
  } else {
    Serial.println("--- SIN reflejo (vacío / negro / lejos)  (D0 = HIGH)");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_SENSOR, INPUT_PULLUP);  // El módulo ya trae pull-up; esto evita lecturas flotantes si se desconecta
  pinMode(PIN_LED, OUTPUT);

  Serial.println();
  Serial.println("==============================================");
  Serial.println("   PRUEBA DE SENSOR INFRARROJO TCRT5000");
  Serial.println("==============================================");
  Serial.print("Sensor bajo prueba: ");
  Serial.println(nombreSensor);
  Serial.print("Leyendo D0 en GPIO ");
  Serial.println(PIN_SENSOR);
  Serial.println("Acerca la mano o una hoja blanca (1-3 cm) para probar.");
  Serial.println("Si nunca cambia: revisa cables o gira el potenciometro.");
  Serial.println("----------------------------------------------");

  estadoEstable = digitalRead(PIN_SENSOR);
  ultimaLectura = estadoEstable;
  digitalWrite(PIN_LED, estadoEstable == LOW ? HIGH : LOW);
  Serial.print("Estado inicial: ");
  imprimirEstado(estadoEstable);
}

void loop() {
  int lectura = digitalRead(PIN_SENSOR);
  unsigned long ahora = millis();

  // Antirrebote: solo aceptamos el cambio si se mantiene un momento
  if (lectura != ultimaLectura) {
    tCambio = ahora;
    ultimaLectura = lectura;
  }

  if ((ahora - tCambio) > ANTIRREBOTE_MS && lectura != estadoEstable) {
    estadoEstable = lectura;

    if (estadoEstable == LOW) {
      detecciones++;
      tInicioDeteccion = ahora;
      digitalWrite(PIN_LED, HIGH);
      Serial.print("[");
      Serial.print(ahora / 1000.0, 2);
      Serial.print(" s] #");
      Serial.print(detecciones);
      Serial.print("  ");
      imprimirEstado(estadoEstable);
    } else {
      digitalWrite(PIN_LED, LOW);
      Serial.print("[");
      Serial.print(ahora / 1000.0, 2);
      Serial.print(" s]     ");
      imprimirEstado(estadoEstable);
      Serial.print("        Duró detectando: ");
      Serial.print(ahora - tInicioDeteccion);
      Serial.println(" ms");
    }
  }

  // Resumen periódico (para confirmar que el ESP32 sigue vivo)
  if (ahora - tResumen >= RESUMEN_MS) {
    tResumen = ahora;
    Serial.print("   ~ [");
    Serial.print(nombreSensor);
    Serial.print("] estado actual: ");
    Serial.print(estadoEstable == LOW ? "DETECTA" : "LIBRE");
    Serial.print(" | detecciones totales: ");
    Serial.println(detecciones);
  }
}
