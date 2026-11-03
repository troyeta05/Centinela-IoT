/*
 * ============================================================
 *  PRUEBA SIMPLE DEL BUZZER ACTIVO
 *  Robot Centinela V7.2 - Banco de pruebas
 * ============================================================
 *
 *  CONEXIÓN:
 *     (+) del buzzer  ------->  GPIO 17 (D17)
 *     (-) del buzzer  ------->  GND
 *
 *  Suena 1 segundo, se apaga 1 segundo, y así se repite.
 *  El LED azul de la placa (GPIO 2) se enciende junto con el buzzer,
 *  así sabes que el ESP32 sí está mandando la señal.
 *
 *  Monitor Serie a 115200 baudios (opcional).
 * ============================================================
 */

const int PIN_BUZZER = 17;
const int PIN_LED    = 2;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  Serial.println("Prueba de buzzer en GPIO 17");
}

void loop() {
  digitalWrite(PIN_BUZZER, HIGH);
  digitalWrite(PIN_LED, HIGH);
  Serial.println("Buzzer ENCENDIDO");
  delay(1000);

  digitalWrite(PIN_BUZZER, LOW);
  digitalWrite(PIN_LED, LOW);
  Serial.println("Buzzer apagado");
  delay(1000);
}
