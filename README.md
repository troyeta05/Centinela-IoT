# Centinela-IoT
# 🤖 Robot Centinela — Firmware IoT

Firmware y software embebido para el **Robot Centinela**, un robot autónomo de tracción diferencial encargado del monitoreo de gases y la navegación doméstica en dos dimensiones.

Este repositorio contiene exclusivamente el código fuente correspondiente a la **capa física, adquisición de datos de sensores y comunicación embebida** del robot basadas en ESP32.

---

## 🛠️ Hardware y Componentes

El firmware interactúa con los siguientes componentes físicos:

* **Unidad de Cómputo Principal:** ESP32 (y ESP32-CAM para captura/stream de video local).
* **Detección de Gases:**
  * `MQ6` (Gas LP y butano)
  * `MQ4` (Gas natural y metano)
* **Condiciones Ambientales:**
  * `DHT22` (Temperatura y humedad relativa)
* **Distancia y Navegación 2D:**
  * `HC-SR04` (Ultrasónico frontal)
  * `VL53L0X` (Láser ToF para barrido de mapa)
* **Obstáculos y Bordes:**
  * `FC51` (Infrarrojo lateral)
  * `TCRT5000` (Infrarrojo de piso/desnivel)
* **Orientación y Control de Movimiento:**
  * `MPU6050` (IMU — Acelerómetro + Giroscopio)
  * Motores DC con encoders para tracción diferencial en chasis de orugas
* **Energía:**
  * Paquete 2S Li-Ion 18650 con BMS

---

## 📡 Comunicación y Control Autónomo

* **Protocolo:** Publicación de telemetría y eventos de alerta vía **MQTT sobre TLS** con nivel de calidad de servicio **QoS 1**.
* **Intervalo de Lectura/Envío:** Transmisión de datos en tiempo real cada **2 segundos**.
* **Lógica Autónoma de Seguridad:** El firmware actúa como la **autoridad principal** de decisión. Si se detecta una concentración de gas peligrosa o un obstáculo crítico, el robot detiene la misión de forma inmediata de manera local, independientemente del estado de la conexión a la nube.

---

## 📁 Estructura del Proyecto

```text
.

```

## 📄 Licencia

Este proyecto está bajo la Licencia **MIT**.