# 🤖 Centinela IoT — Robot autónomo detector de gases

![Plataforma](https://img.shields.io/badge/plataforma-ESP32-blue)
![Lenguaje](https://img.shields.io/badge/firmware-Arduino%20C%2B%2B-00979D)
![Backend](https://img.shields.io/badge/backend-Supabase%20%2F%20PostgreSQL-3ECF8E)
![Versión](https://img.shields.io/badge/robot-V7.2-orange)
![Licencia](https://img.shields.io/badge/licencia-MIT-green)

**Centinela** es un robot de tracción diferencial sobre orugas que patrulla un hogar de forma autónoma, mide concentraciones de **gas LP y metano**, temperatura y humedad, y envía su telemetría a la nube cada 2 segundos. Si detecta una fuga crítica, se detiene y queda en monitoreo **por decisión local**, aunque no haya conexión a internet.

Este repositorio reúne todo el proyecto: firmware del ESP32, pruebas de cada componente, base de datos en Supabase, piezas para impresión 3D y documentación técnica.

---

## 📑 Contenido

- [Características](#-características)
- [Arquitectura](#-arquitectura)
- [Hardware](#-hardware)
- [Mapa de pines](#-mapa-de-pines)
- [Comportamiento autónomo](#-comportamiento-autónomo)
- [Telemetría y base de datos](#-telemetría-y-base-de-datos)
- [Puesta en marcha](#-puesta-en-marcha)
- [Estructura del repositorio](#-estructura-del-repositorio)
- [Impresión 3D](#-impresión-3d)
- [Documentación](#-documentación)
- [Licencia](#-licencia)

---

## ✨ Características

- **Navegación 100 % autónoma** con máquina de estados de 12 comportamientos (patrullaje, seguimiento de pared, evasión, anti-caídas, desatasco, etc.).
- **Doble núcleo con FreeRTOS:** el control corre a 50 Hz en un núcleo y la telemetría en el otro, así el robot nunca se queda "ciego" mientras transmite.
- **Detección de gases** con sensores MQ-6 (LP/butano) y MQ-4 (metano) y paro de emergencia con histéresis.
- **Protección anti-caídas** con 4 sensores IR de piso y detección de robot volteado o levantado.
- **Odometría diferencial** (x, y, θ) con encoders magnéticos.
- **Telemetría HTTPS** a Supabase con esquema JSON versionado (v1.1), marca de tiempo NTP y número de secuencia.
- **Backend con seguridad por filas (RLS)**, catálogo de canales, calibración de sensores MQ y gestión de alertas.

---

## 🏗️ Arquitectura

```text
┌────────────────────────── ESP32 ──────────────────────────┐
│  Núcleo 1 — loop() cada 20 ms (50 Hz)                     │
│   Sensores → Máquina de estados → Rampa PWM → L298N       │
│   Odometría · Anti-caídas · Evasión · Paro por gas        │
│                                                           │
│  Núcleo 0 — Tarea de telemetría cada 2000 ms              │
│   NTP (UTC) → JSON v1.1 → HTTPS POST                      │
└──────────────────────────────┬────────────────────────────┘
                               │  RPC ingest_telemetry
                               ▼
              ┌──────────── Supabase ────────────┐
              │ PostgreSQL + RLS                 │
              │ frames · lecturas · poses ·      │
              │ estados · batería · alertas      │
              └────────────────┬─────────────────┘
                               ▼
                     Aplicación web / móvil
```

---

## 🛠️ Hardware

| Función                   | Componente                                                                                  |
| ------------------------- | ------------------------------------------------------------------------------------------- |
| Microcontrolador          | ESP32 (core Arduino 2.x o 3.x)                                                              |
| Gas LP / propano / butano | MQ-6                                                                                        |
| Metano / gas natural      | MQ-4                                                                                        |
| Temperatura y humedad     | DHT22                                                                                       |
| Distancia frontal         | HC-SR04 (ultrasónico, ECHO con divisor a 3.3 V)                                             |
| Piso / desniveles         | 4 × IR TCRT5000 (2 frontales, 2 traseros)                                                   |
| Paredes laterales         | 2 × IR (módulo LM393)                                                                       |
| Tracción                  | 2 × motor GM25-370 con encoder Hall · puente H L298N                                        |
| Chasis                    | Orugas, llanta de 67 mm, vía de 17.5 cm (piezas impresas en 3D)                             |
| Energía                   | Baterías Li-Ion 18650 con BMS · elevador MT3608 · divisor 100 kΩ / 27 kΩ para medir batería |

> Las pruebas incluyen un sketch para **MQ-3** y un **buzzer**, pero el firmware principal no los usa. El canal `mq7_raw` está reservado en el esquema como `not_installed`.

---

## 📌 Mapa de pines

| Pin ESP32 | Conexión               | Pin ESP32 | Conexión                     |
| --------- | ---------------------- | --------- | ---------------------------- |
| 36 (VP)   | Divisor de batería     | 25        | IR piso frontal izq.         |
| 34        | MQ-6 (A0)              | 26        | IR piso frontal der.         |
| 35        | MQ-4 (A0)              | 27        | IR piso trasero izq.         |
| 4         | DHT22 (DATA)           | 14        | IR piso trasero der.         |
| 5         | HC-SR04 TRIG           | 13        | IR lateral izq.              |
| 18        | HC-SR04 ECHO           | 16        | IR lateral der.              |
| 19        | Encoder motor izq. (A) | 21 / 22   | L298N IN1 / IN2 (motor izq.) |
| 23        | Encoder motor der. (A) | 32 / 33   | L298N IN3 / IN4 (motor der.) |

Los sensores analógicos usan **ADC1** para no chocar con el WiFi. El diagrama completo está en [`Documentación/Diagrama Conexiones Protoboard.html`](Documentación/Diagrama%20Conexiones%20Protoboard.html).

---

## 🧭 Comportamiento autónomo

| Estado                                     | Qué hace                                                   |
| ------------------------------------------ | ---------------------------------------------------------- |
| `BOOT_WAIT`                                | Espera 2 s al encender                                     |
| `PATROL_CRUISE`                            | Avance libre (PWM 185)                                     |
| `WALL_FOLLOWING`                           | Sigue paredes con los IR laterales                         |
| `AVOID_OBSTACLE`                           | Esquiva obstáculos frontales (< 32 cm; reversa si < 15 cm) |
| `CLIFF_FRONT_ESCAPE` / `CLIFF_REAR_ESCAPE` | Escapa de escalones delante o detrás                       |
| `STEEP_CLIMB_BOOST`                        | Torque extra en rampas y alfombras (PWM 240)               |
| `UNSTUCK_MANEUVER`                         | Desatasco multi-etapa usando encoders                      |
| `TRAPPED_COOLDOWN`                         | Pausa protectora tras 4 intentos fallidos                  |
| `FLIPPED_EMERGENCY`                        | Corta motores si está volteado o levantado                 |
| `GAS_EMERGENCY_STOP`                       | Fuga crítica (ADC ≥ 2700): paro y monitoreo                |
| `LOW_BATTERY_PARKED`                       | Se estaciona con batería baja (< 3.25 V)                   |

Los umbrales están en la sección 3 de [`centinela_Autonomo.ino`](Firmware/centinela_Autonomo.ino) y se pueden ajustar.

---

## 📡 Telemetría y base de datos

Cada 2 s el robot envía un JSON (esquema **v1.1**) por **HTTPS POST** a la función RPC `ingest_telemetry` de Supabase. Incluye identificadores de dispositivo, misión y mapa, número de secuencia, hora sincronizada por NTP, pose estimada y lecturas de todos los canales con su calidad. Ejemplo completo: [`Documentación/JSON_estructura.json`](Documentación/JSON_estructura.json).

El script [`Base de Datos/Centinela Basse de Datos.sql`](Base%20de%20Datos/Centinela%20Basse%20de%20Datos.sql) crea 16 tablas (hogares, robots, misiones, frames, lecturas, poses, alertas, calibraciones…), funciones de calibración para sensores MQ (`mq_estimate`, `calibrate_r0_clean_air`) y políticas RLS por hogar. Diagrama: [`Diagrama ER.png`](Base%20de%20Datos/Diagrama%20ER.png).

---

## 🚀 Puesta en marcha

1. **Base de datos:** crea un proyecto en [Supabase](https://supabase.com) y ejecuta el script SQL en el _SQL Editor_. Da de alta un dispositivo, una misión y una versión de mapa.
2. **Entorno:** instala Arduino IDE con el core **ESP32 de Espressif** y la librería **DHT sensor library** (Adafruit).
3. **Configura** la sección 1 del firmware:
   ```cpp
   const char* WIFI_SSID      = "TU_RED_WIFI";
   const char* WIFI_PASSWORD  = "TU_PASSWORD_WIFI";
   const char* SUPABASE_URL   = "https://xxxx.supabase.co";
   const char* SUPABASE_KEY   = "...";
   const char* DEVICE_ID      = "...";  // UUIDs de tu base de datos
   const char* MISSION_ID     = "...";
   const char* MAP_VERSION_ID = "...";
   ```
4. **Prueba cada componente** con los sketches de [`Firmware/Pruebas de Componentes`](Firmware/Pruebas%20de%20Componentes) antes de montar todo.
5. **Carga** `centinela_Autonomo.ino` y revisa el monitor serie.

> ⚠️ **No subas tus credenciales reales al repositorio.** La clave `service_role` de Supabase da acceso total a la base de datos.

---

## 📁 Estructura del repositorio

```text
Centinela-IoT/
├── Firmware/
│   ├── centinela_Autonomo.ino        # Firmware principal (v7.2)
│   └── Pruebas de Componentes/       # Un sketch por sensor/actuador
├── Base de Datos/
│   ├── Centinela Basse de Datos.sql  # Esquema, funciones y RLS
│   └── Diagrama ER.(png|svg)
├── Impresion 3D/
│   ├── STL por Bandeja/              # P01–P12, listas para laminar
│   ├── Proyecto de Impresion 3D/     # Proyecto .3mf completo
│   └── Diagramas/                    # Guía de soportes
├── Documentación/                    # PDFs técnicos, diagrama de conexiones, JSON
├── LICENSE
└── README.md
```

---

## 🖨️ Impresión 3D

Las piezas están agrupadas en 12 bandejas (`P01 Carcasa` … `P12 Gomas de Contacto`): carcasa, tapa, marcos, biseles, transmisión, alojamientos, fijaciones, eslabones de oruga, pasadores y gomas. El proyecto completo está en formato `.3mf` y en `Diagramas/` hay una guía de soportes.

---

## 📚 Documentación

| Documento                                                                                                   | Contenido                        |
| ----------------------------------------------------------------------------------------------------------- | -------------------------------- |
| [Documentación Técnica (final)](Documentación/Documentacion%20Técnica%20Centinela%20Final.pdf)              | Diseño completo del sistema      |
| [Manual Rover V7.2](Documentación/Documentacion%20y%20manual%20Rover%20V7%202.pdf)                          | Ensamblaje y uso                 |
| [Consumo de datos web y móvil](Documentación/Consumo%20de%20la%20Datos%20Web%20y%20Movil.pdf)               | Cómo las apps leen los datos     |
| [Expediente de revisión](Documentación/Centinela%20Expediente%20revision%20matematica%20y%20gramatical.pdf) | Revisión matemática y gramatical |

---

## 📄 Licencia

Distribuido bajo la licencia **MIT**. © 2026 Alexis León. Consulta [LICENSE](LICENSE).
