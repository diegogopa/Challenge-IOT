# 💧 Sistema IoT para el monitoreo de disponibilidad y escasez de agua

Prototipo de Internet de las Cosas (IoT) basado en **ESP32** para el monitoreo local, en tiempo real y **sin conexión a Internet**, del nivel de agua y variables ambientales asociadas a condiciones de sequía o desborde, en la región de Sabana Centro, Cundinamarca.

Proyecto desarrollado para el curso de **Internet de las Cosas – 2026-2**, Facultad de Ingeniería, **Universidad de La Sabana**.

📖 **Documentación completa:** ver la [Wiki del proyecto](../../wiki)

---

## 🧠 Descripción

El sistema combina un sensor ultrasónico, un sensor de temperatura/humedad y un sensor de radiación UV para:

- Estimar el porcentaje de nivel de agua disponible en un tanque o recipiente.
- Calcular la **tendencia** del nivel (sube / baja / estable).
- Clasificar la radiación UV en cuatro categorías (BAJA, MEDIA, ALTA, EXTREMA).
- Clasificar el estado del sistema en **OK**, **SEQUÍA** o **DESBORDE**, según el nivel de agua.
- Mostrar toda la información en una pantalla OLED con una interfaz animada.
- Activar una alarma sonora (buzzer) intermitente cuando el estado es crítico.

Todo el procesamiento ocurre localmente en el ESP32: no requiere Wi-Fi, Bluetooth, servidores ni servicios en la nube.

---

## 🔩 Hardware utilizado

| Componente        | Función                              |
| ------------------ | ------------------------------------- |
| ESP32 DevKit V1     | Procesamiento principal              |
| HC-SR04             | Medición de distancia / nivel de agua |
| DHT22                | Temperatura y humedad                |
| GUVA-S12SD           | Radiación ultravioleta (analógico)   |
| OLED I2C (SH1106)    | Visualización                        |
| Buzzer activo        | Alarma sonora                        |
| Protoboard + cables Dupont | Montaje (sin resistencias ni divisores de tensión) |

Conexión de pines, diagramas y detalle completo del montaje: ver [Diseño de hardware](../../wiki/06.-Diseno-de-hardware) en la wiki.

---

## ⚙️ Software

- **IDE:** Arduino IDE
- **Placa:** ESP32 Dev Module
- **Librerías:**
  - `DHT sensor library`
  - `Adafruit GFX Library`
  - `Adafruit SH110X`

El código fuente completo está en [`Sistema_IoT_Agua.ino`](./Sistema_IoT_Agua.ino).

### Lógica del sistema

- El **nivel de agua** se calcula a partir de la distancia medida por el HC-SR04.
- El **estado** del sistema depende únicamente del nivel:
  - `Nivel ≥ 95 %` → **DESBORDE**
  - `Nivel ≤ 20 %` → **SEQUÍA**
  - En cualquier otro caso → **OK**
- Temperatura, humedad y radiación UV se muestran como información complementaria en pantalla.
- Cuando el estado no es OK, el buzzer parpadea cada 400 ms y el encabezado del OLED se invierte mostrando el mensaje de alerta.

Detalle completo de la lógica: ver [Lógica de decisión y clasificación de estados](../../wiki/08.-Logica-de-fusion).

---

## 🖥️ Interfaz OLED

La pantalla se organiza en tres columnas:

1. **Nivel de agua** – barra vertical animada con el porcentaje actual.
2. **Sensores** – iconos de temperatura, humedad y voltaje UV.
3. **Tendencia y UV** – flecha de tendencia del nivel y barra de severidad de radiación UV.

Al encender el sistema se muestra una animación de arranque ("AQUA MONITOR" + barra de progreso).

---

## 🚀 Cómo usarlo

1. Clona este repositorio.
2. Abre `Sistema_IoT_Agua.ino` en Arduino IDE.
3. Instala las librerías mencionadas arriba desde el gestor de librerías de Arduino.
4. Selecciona la placa **ESP32 Dev Module** y el puerto correspondiente.
5. Ajusta, si es necesario, los valores de calibración del tanque:

   ```cpp
   #define DIST_TANQUE_LLENO 2.0
   #define DIST_TANQUE_VACIO 20.0
   ```

6. Carga el código al ESP32 y realiza el montaje según el [diagrama de conexiones](../../wiki/06.-Diseno-de-hardware).

---

## 📁 Estructura del repositorio

```text
.
├── Sistema_IoT_Agua.ino     # Código fuente principal
├── README.md                # Este archivo
└── docs/                    # Material fotográfico y video (opcional)
```

---

## 👥 Equipo

| Integrante            | Rol principal                                   |
| ---------------------- | ------------------------------------------------ |
| Victor Andres Luna      | Desarrollo de hardware e integración             |
| Federico Valdez Munoz   | Desarrollo de software y pruebas                 |
| Diego Alejandro Gómez   | Diseño, documentación e integración del sistema  |

**Profesor:** Andres Felipe Beltran
**Institución:** Universidad de La Sabana – Facultad de Ingeniería
**Periodo:** 2026-2

---

## 📚 Documentación completa

Toda la documentación del proyecto (contexto, requisitos, arquitectura, pruebas, modelo de negocio, uso de IA, etc.) está organizada en la **[Wiki](../../wiki)** de este repositorio.

## 📄 Licencia

Proyecto académico desarrollado con fines educativos para el curso de Internet de las Cosas de la Universidad de La Sabana.
