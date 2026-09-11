# 💧 AquaMonitor — Sistema IoT de Monitoreo de Disponibilidad y Escasez de Agua

![Platform](https://img.shields.io/badge/platform-ESP32-blue)
![Language](https://img.shields.io/badge/language-C%2B%2B%20(Arduino)-orange)
![License](https://img.shields.io/badge/license-Academic-lightgrey)
![Status](https://img.shields.io/badge/status-Prototipo%20funcional-brightgreen)

Prototipo funcional de Internet de las Cosas (IoT) para el monitoreo **local** de condiciones asociadas a la disponibilidad y escasez de agua en puntos críticos de almacenamiento de la región de Sabana Centro, Cundinamarca. Todo el procesamiento ocurre en el propio microcontrolador: no depende de Wi-Fi, Bluetooth, Internet ni servicios en la nube para detectar riesgos y generar alertas.

📖 Documentación completa del proyecto: **[Wiki del repositorio](../../wiki)**

---

## 👥 Equipo

**Universidad de La Sabana — Facultad de Ingeniería**
Internet de las Cosas · 2026-2

| Rol | Nombre |
|---|---|
| Integrante | Victor Andrés Luna |
| Integrante | Federico Valdez Muñoz |
| Integrante | Diego Alejandro Gómez |
| Profesor | Andrés Felipe Beltrán |

---

## 📌 Resumen

El sistema utiliza un **ESP32 DevKit V1** como unidad central de procesamiento e integra tres sensores para obtener información sobre el nivel de agua y las condiciones ambientales de un tanque o recipiente de almacenamiento:

| Sensor | Variable | Rol en el sistema |
|---|---|---|
| **HC-SR04** | Distancia a la superficie del agua | Estima el porcentaje de nivel del tanque |
| **DHT22** | Temperatura y humedad relativa | Contexto ambiental / indicador de evaporación |
| **GUVA-S12SD** | Radiación UV (señal analógica) | Categoría de riesgo UV / indicador de evaporación |

El estado de alarma del sistema depende del **nivel de agua**, y se muestra en tiempo real en una pantalla **OLED I2C** (SH1106G), con un **buzzer activo** que avisa de forma sonora cuando se alcanza una condición de riesgo.

---

## 🚦 Estados del sistema

| Estado | Condición |
|---|---|
| 🔵 **OK** | Nivel entre 20% y 95% |
| 🟡 **SEQUIA** | Nivel ≤ 20% |
| 🔴 **DESBORDE** | Nivel ≥ 95% |

Mientras el estado sea distinto de `OK`, el buzzer parpadea cada 400 ms y el encabezado del OLED se invierte mostrando el mensaje de alerta correspondiente.

---

## 🆕 Novedad: indicador informativo de evaporación potencial

Se agregó un **índice de evaporación (0-100)** que fusiona temperatura, humedad relativa y radiación UV, inspirado en una adaptación simplificada del método **Hargreaves-Samani** de estimación de evapotranspiración (ver [Bibliografía](#-bibliografía)).

```
Índice de evaporación = (Temperatura × 0.40) + (UV × 0.30) + (Humedad × 0.30)
```

Este índice se muestra en el OLED como **"EVP:xx%"** junto a la categoría UV. Es un dato **puramente informativo**, con el mismo rol que ya tenían la radiación UV y la tendencia de nivel: **no altera el estado de alarma ni el buzzer**, siguiendo la misma filosofía de diseño documentada en la sección *8.1* de la wiki.

Adicionalmente, el firmware calcula en segundo plano:

- **Tasa real de cambio de nivel** (%/hora), medida en ventanas de 2 minutos.
- **Pérdida esperada por evaporación** (%/hora), derivada del índice.
- **Pérdida extra**, la diferencia entre ambas — reportada por Serial como base para un futuro modelo predictivo de fugas o consumo anómalo (ver [Trabajo futuro](#-trabajo-futuro)).

📄 Detalle completo del modelo: [08. Lógica de fusión — sección 8.8](../../wiki/08.-Logica-de-fusion) · [Anexos — Bibliografía](../../wiki/15.-Anexos)

---

## 🔌 Componentes de hardware

| Componente | Pin ESP32 |
|---|---|
| DHT22 (datos) | GPIO 4 |
| GUVA-S12SD (analógico) | GPIO 34 |
| HC-SR04 (TRIG) | GPIO 5 |
| HC-SR04 (ECHO) | GPIO 18 |
| Buzzer activo | GPIO 33 |
| OLED SH1106G (I2C SDA) | GPIO 21 |
| OLED SH1106G (I2C SCL) | GPIO 23 |

---

## 🖥️ Entorno de desarrollo

- **IDE:** Arduino IDE
- **Placa:** `ESP32 Dev Module`
- **Librerías requeridas:**
  - `DHT sensor library`
  - `Adafruit GFX Library`
  - `Adafruit SH110X`

---

## ⚙️ Funcionamiento general

1. Muestra la animación de arranque ("AQUA MONITOR" + barra de progreso).
2. Cada 1.5 s, lee temperatura, humedad, voltaje UV y distancia.
3. Calcula el nivel porcentual de agua y su tendencia respecto a la lectura anterior.
4. Clasifica la radiación UV según el voltaje leído.
5. Calcula el índice informativo de evaporación potencial.
6. Determina el estado del sistema (`OK` / `SEQUIA` / `DESBORDE`) según el nivel de agua.
7. Si el estado no es `OK`, hace parpadear el buzzer y el encabezado del OLED.
8. Actualiza continuamente el OLED: barra de nivel animada, iconos, tendencia, categoría UV, índice de evaporación e indicador de actividad.
9. Envía todas las lecturas al monitor serial para depuración (115200 baudios).

---

## 📁 Estructura del repositorio

```
Challenge-IOT/
├── AquaMonitor.ino     # Firmware principal (ESP32)
├── README.md           # Este archivo
└── docs/                # Diagramas y evidencia fotográfica (si aplica)
```

---

## 🗺️ Documentación (Wiki)

| # | Página |
|---|---|
| 01 | [Información del proyecto](../../wiki/01.-Informacion-del-proyecto) |
| 02 | [Contexto, problema y objetivos](../../wiki/02.-Contexto-problema-y-objetivos) |
| 03 | [Requisitos y restricciones](../../wiki/03.-Requisitos-y-restricciones) |
| 04 | [Componentes y selección tecnológica](../../wiki/04.-Componentes-y-seleccion-tecnologica) |
| 05 | [Arquitectura del sistema](../../wiki/05.-Arquitectura-del-sistema) |
| 06 | [Diseño de hardware](../../wiki/06.-Diseno-de-hardware) |
| 07 | [Diseño de software](../../wiki/07.-Diseno-de-software) |
| 08 | [Lógica de fusión](../../wiki/08.-Logica-de-fusion) |
| 09 | [Implementación](../../wiki/09.-Implementacion) |
| 10 | [Configuración experimental](../../wiki/10.-Configuracion-experimental) |
| 11 | [Pruebas y resultados](../../wiki/11.-Pruebas-y-resultados) |
| 12 | [Autoevaluación](../../wiki/12.-Autoevaluacion) |
| 13 | [Modelo de negocio](../../wiki/13.-Modelo-de-negocio) |
| 14 | [Conclusiones y trabajo futuro](../../wiki/14.-Conclusiones-y-trabajo-futuro) |
| 15 | [Anexos](../../wiki/15.-Anexos) |

---

## 🔭 Trabajo futuro

- Incorporar un sensor de caudal.
- Incorporar un sensor de presión atmosférica.
- Mejorar la calibración del sensor UV.
- Incorporar un sistema de alimentación solar.
- Añadir almacenamiento de datos.
- Incorporar comunicación LoRa.
- Implementar múltiples estaciones.
- Desarrollar una plataforma de monitoreo remoto.
- **Implementar modelos predictivos** — *primer avance: indicador informativo de evaporación (Hargreaves-Samani).*
- Mejorar la precisión de la estimación del riesgo, evolucionando el indicador de evaporación hacia una detección activa de fugas o consumo anómalo.

---

## 📚 Bibliografía

- Hargreaves, G. H., & Samani, Z. A. (1985). Reference crop evapotranspiration from temperature. *Applied Engineering in Agriculture, 1*(2), 96–99.
- Allen, R. G., Pereira, L. S., Raes, D., & Smith, M. (1998). *Crop Evapotranspiration — Guidelines for Computing Crop Water Requirements* (FAO Irrigation and Drainage Paper 56). FAO, Rome. https://www.fao.org/4/x0490e/x0490e00.htm
- Hargreaves, G. H., & Allen, R. G. (2003). History and evaluation of Hargreaves evapotranspiration equation. *Journal of Irrigation and Drainage Engineering, 129*(1), 53–63. https://doi.org/10.1061/(ASCE)0733-9437(2003)129:1(53)
- Genicom Co., Ltd. (2011). *GUVA-S12SD UV-B Sensor — Technical Data*. https://cdn-shop.adafruit.com/datasheets/1918guva.pdf
- Espressif Systems. *ESP32 Series Datasheet*. https://www.espressif.com/en/products/socs/esp32
- Adafruit Industries. *Adafruit_SH110X Arduino Library*. https://github.com/adafruit/Adafruit_SH110X

> **Nota:** la ecuación original de Hargreaves-Samani usa temperatura máxima/mínima y radiación extraterrestre por latitud, no un sensor UV directo. El índice implementado en este proyecto es una heurística inspirada en esa lógica, adaptada a los sensores disponibles en el prototipo — no la fórmula FAO-56 aplicada de forma literal.

---

## 📄 Licencia

Proyecto académico desarrollado para el curso de Internet de las Cosas — Universidad de La Sabana, 2026-2.
