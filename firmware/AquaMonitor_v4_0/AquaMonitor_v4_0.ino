#include "DHT.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_BMP085.h>   // "Adafruit BMP085 Library" (sirve para BMP180)
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>      // "PubSubClient" de Nick O'Leary (cliente MQTT)
#include <ESPmDNS.h>           // resolver "aquamonitor.local" (hotspot sin IP fija)

// ============================================================
// AQUAMONITOR v4.0  (Challenge #3: ecosistema IoT local + nube)
// Cambios respecto a v3.2:
//  - Cliente MQTT hacia el Gateway (Raspberry Pi + Mosquitto) en una
//    tarea FreeRTOS propia (tareaMQTT), independiente de la medicion
//    y del loop principal.
//  - Publica telemetria cada 10 s, eventos (cambios de estado, fallas,
//    silenciado) apenas ocurren y su estado online/offline (LWT).
//  - Recibe la orden de silenciar desde la nube (Ubidots -> Gateway).
//  - Recibe del Gateway el pronostico y la recomendacion de la IA y los
//    muestra en el tablero local.
//  - Nueva falla vigilada: conexion con el Gateway (F_MQTT, sin buzzer).
//  - Correcciones: el filtro por subred ya no deja pasar a nadie si se
//    pierde la WLAN; una lectura UV de 0 mV ya no se toma como falla
//    (de noche es un valor real).
//
// Historial v3.2:
//  - Deteccion de desconexion/falla por sensor: DHT22, BMP180, GUVA,
//    ultrasonico, OLED y WiFi. Aviso en OLED, tablero, registro de
//    eventos y chirrido del buzzer (silenciable desde el tablero).
//  - Recuperacion automatica: si el sensor vuelve, se reinicializa solo.
//  - Rango de medicion del nivel: 4 cm (100 %) a 9 cm (0 %) en el modelo a escala.
//  - Distancia en cm visible en el tablero y en el monitor serie.
// Cambios v3.2-logo (solo diseno del tablero web):
//  - Logo de AquaMonitor (gota con nivel) en el encabezado y como
//    icono de la pestana del navegador. La logica no cambia.
// ============================================================

// ---------------- CONFIGURACION ----------------
const char* WIFI_SSID = "AquaMonitor";
const char* WIFI_PASS = "12345678";
const char* WEB_USER  = "admin";
const char* WEB_PASS  = "cambiame";

#define WIFI_TIMEOUT_MS 15000UL
#define WIFI_RETRY_MS   10000UL

// ---- MQTT hacia el Gateway IoT (Raspberry Pi con Mosquitto) ----
const char* MQTT_HOST   = "aquamonitor.local"; // nombre (mDNS) o IP de la Raspberry Pi en la WLAN
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER   = "esp32_est01";      // usuario creado en Mosquitto (mosquitto_passwd)
const char* MQTT_PASS   = "EspAqua2026";
const char* ESTACION_ID = "est01";            // identificador de la estacion (temas MQTT)

#define PERIODO_TELEMETRIA_MS 10000UL         // telemetria cada 10 s
#define MQTT_RETRY_MS         5000UL          // reintento de conexion al broker
#define MQTT_BUFFER_BYTES     1024

AsyncWebServer servidorWeb(80);

// ---------------- PINES Y SENSORES ----------------
#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

#define GUVA_PIN 34
#define UV_V_POR_INDICE 0.1f
#define UV_V_OFFSET     0.14f       // ajusta con el UVv medido con el sensor tapado
#define UV_RUIDO_MAX_MV 150         // dispersion maxima entre 16 muestras (si no: pin flotante)
#define UV_V_SATURADO   1.20f       // por encima no es UV real (indice >11) -> falla

#define TRIG_PIN 5
#define ECHO_PIN 18                 // ECHO a 5 V: conectar con divisor 1k (serie) / 2k (a GND)
// ---- Rango de nivel: 4 cm = 100 %  ...  9 cm = 0 % (ajustar a la geometria del tanque) ----
#define DIST_TANQUE_LLENO 4.0f
#define DIST_TANQUE_VACIO 9.0f
#define DIST_MIN_VALIDA   3.0f
#define DIST_MAX_VALIDA   25.0f     // mas lejos que esto se considera eco invalido
#define N_MUESTRAS_DIST   5
#define NIVEL_EMA_ALFA    0.5f      // 1.0 = sin filtro (respuesta inmediata)

#define BUZZER_PIN 33

#define SDA_PIN 21
#define SCL_PIN 23

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C
#define BMP_ADDR  0x77
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledOk = false;

Adafruit_BMP085 bmp;
bool bmpIniciado = false;

// ---- Calibracion (valor_cal = a*lectura + b) ----
#define CAL_T_A 1.0f
#define CAL_T_B 0.0f
#define CAL_H_A 1.0f
#define CAL_H_B 0.0f
#define CAL_P_A 1.0f
#define CAL_P_B 0.0f

// ---------------- PARAMETROS DE FUSION ----------------
#define PERIODO_SENSADO_MS   2000UL
#define VENTANA_TASA_MS      300000UL   // 60000 para pruebas aceleradas
#define TENDENCIA_MS         10000UL
#define TASA_EVAP_MAX_PCT_HORA 4.0f
#define UMBRAL_FUGA_EXTRA_PCT_HORA -2.5f

#define NIVEL_DESBORDE_ON  95.0f
#define NIVEL_DESBORDE_OFF 90.0f
#define NIVEL_SEQUIA_ON    20.0f
#define NIVEL_SEQUIA_OFF   25.0f
#define NIVEL_RIESGO       35.0f
#define EVAP_RIESGO        70.0f

#define PRES_VENTANA_MS    10800000UL   // 3 h (60000 para pruebas)
#define PRES_CAIDA_HPA     2.0f
#define EVAP_RIESGO_PRES   55.0f

// ---- Deteccion de fallas ----
#define FALLAS_NIVEL_MAX    5           // ciclos sin eco -> ERROR_SENSOR (~12 s)
#define FALLAS_SENSOR_MAX   3           // ciclos malos seguidos -> falla (~6 s)
#define CHEQUEO_OLED_MS     3000UL

// Bits de falla
#define F_DHT    0x01
#define F_BMP    0x02
#define F_UV     0x04
#define F_ULTRA  0x08
#define F_OLED   0x10
#define F_WIFI   0x20
#define F_MQTT   0x40                                          // sin conexion con el Gateway
#define N_BITS_FALLA 7
#define F_SONORA   (F_DHT | F_BMP | F_UV | F_ULTRA | F_OLED)   // hacen sonar el buzzer
#define F_EN_OLED  (F_DHT | F_BMP | F_UV)                      // se muestran en la pantalla

// ---------------- TIPOS ----------------
enum Estado : uint8_t { EST_OK = 0, EST_DESBORDE, EST_FUGA, EST_SEQUIA, EST_ERROR_SENSOR };

const char* NOMBRE_ESTADO[] = { "OK", "DESBORDE", "FUGA", "SEQUIA", "ERROR_SENSOR" };
const char* TEXTO_ALARMA[]  = { "", "!! DESBORDE !!", "!! FUGA !!", "!! SEQUIA !!", "! SENSOR !" };
const char* CAT_UV[]        = { "BAJA", "MEDIA", "ALTA", "MUY ALTA", "EXTREMA" };
const char* CAT_UV_CORTA[]  = { "BAJA", "MEDI", "ALTA", "MALT", "EXTR" };

// Codigos de evento: 0-4 estados, 5-11 fallas, 12-18 recuperaciones, 19-20 silenciado
#define EV_FALLA_BASE      5
#define EV_RECUP_BASE      (EV_FALLA_BASE + N_BITS_FALLA)
#define EV_SILENCIO_LOCAL  19
#define EV_SILENCIO_NUBE   20
const char* NOMBRE_EVENTO[] = {
  "OK", "DESBORDE", "FUGA", "SEQUIA", "ERROR_SENSOR",
  "FALLA_DHT22", "FALLA_BMP180", "FALLA_UV", "FALLA_ULTRASONICO", "FALLA_OLED", "FALLA_WIFI", "FALLA_MQTT",
  "RECUPERADO_DHT22", "RECUPERADO_BMP180", "RECUPERADO_UV", "RECUPERADO_ULTRASONICO",
  "RECUPERADO_OLED", "RECUPERADO_WIFI", "RECUPERADO_MQTT",
  "SILENCIADA_LOCAL", "SILENCIADA_NUBE"
};

struct Datos {
  float nivel, distCm, temp, hum, uvV, uvIdx, evap;
  float tasaReal, tasaEsperada, perdidaExtra;
  float presion, tempBmp, presDelta;
  int8_t tendencia;
  uint8_t catUV;
  uint8_t fallas;
  Estado estado;
  bool silenciada, presionCae;
  // ---- Recibido del Gateway (modulo de IA) ----
  bool  iaValida, iaAnomalia;
  float iaHoras;               // horas hasta nivel critico (-1 estable, -2 sin datos suficientes)
  char  iaReco[200];           // recomendacion para la autoridad
};

Datos datos = {};

#define TAM_HISTORICO 60
#define INTERVALO_HISTORICO_MS 60000UL

struct Muestra {
  unsigned long t;
  float nivel, temp, hum, uvIdx, evap, presion;
  Estado estado;
};

Muestra historico[TAM_HISTORICO];
int idxHistorico = 0, totalHistorico = 0;
unsigned long ultimoRegistroHist = 0;

#define MAX_EVENTOS 10
struct Evento { unsigned long t; uint8_t codigo; };
Evento eventos[MAX_EVENTOS];
int idxEventos = 0, totalEventos = 0;

SemaphoreHandle_t mutexDatos;
SemaphoreHandle_t mutexI2C;
QueueHandle_t colaEventos;      // eventos pendientes de publicar por MQTT
uint32_t seqTelemetria = 0;     // contador para medir perdida de mensajes en el Gateway

// ---------------- ESTADO LOCAL DE loop() ----------------
unsigned long ultimoBeep = 0;
bool buzzerEncendido = false;
int frameAnimacion = 0;
unsigned long ultimoFrame = 0;
bool puntoVivoVisible = true;

// ---------------- UTILIDADES ----------------
float mapFloat(float x, float inMin, float inMax, float outMin, float outMax) {
  if (inMax == inMin) return outMin;
  float t = (x - inMin) / (inMax - inMin);
  t = constrain(t, 0.0f, 1.0f);
  return outMin + t * (outMax - outMin);
}

float calcularIndiceEvaporacion(float t, float h, float uvIdx) {
  float compTemp = mapFloat(t, 15.0f, 40.0f, 0, 100);
  float compHum  = constrain(100.0f - h, 0.0f, 100.0f);
  float compUV   = mapFloat(uvIdx, 0.0f, 11.0f, 0, 100);
  return constrain(compTemp * 0.40f + compUV * 0.30f + compHum * 0.30f, 0.0f, 100.0f);
}

uint8_t categoriaUV(float uvIdx) {
  if (uvIdx < 3.0f)  return 0;
  if (uvIdx < 6.0f)  return 1;
  if (uvIdx < 8.0f)  return 2;
  if (uvIdx < 11.0f) return 3;
  return 4;
}

// Lee el GUVA. Devuelve false si la lectura es sospechosa (pin flotante o saturado).
// 0 mV es valido: de noche o con el sensor tapado el GUVA entrega ~0 V.
bool leerUV(float &voltios) {
  uint32_t suma = 0, mn = 100000, mx = 0;
  for (int i = 0; i < 16; i++) {
    uint32_t x = analogReadMilliVolts(GUVA_PIN);
    suma += x;
    if (x < mn) mn = x;
    if (x > mx) mx = x;
    delayMicroseconds(200);
  }
  voltios = (suma / 16.0f) / 1000.0f;
  bool ruido = (mx - mn) > UV_RUIDO_MAX_MV;
  bool saturado = voltios > UV_V_SATURADO;
  return !(ruido || saturado);
}

float medirDistanciaUnica(float velCmPorUs) {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long dur = pulseIn(ECHO_PIN, HIGH, 30000);
  if (dur == 0) return -1;
  return dur * velCmPorUs / 2.0f;
}

float medirDistanciaMediana(float tempC) {
  float vel = 0.03313f + 0.0000606f * tempC;
  float m[N_MUESTRAS_DIST];
  int n = 0;
  for (int i = 0; i < N_MUESTRAS_DIST; i++) {
    float d = medirDistanciaUnica(vel);
    if (d >= DIST_MIN_VALIDA && d <= DIST_MAX_VALIDA) m[n++] = d;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (n < 3) return -1;
  for (int i = 1; i < n; i++) {
    float k = m[i]; int j = i - 1;
    while (j >= 0 && m[j] > k) { m[j + 1] = m[j]; j--; }
    m[j + 1] = k;
  }
  return m[n / 2];
}

Estado calcularEstado(Estado prev, float nivel, float evap, float tasa, float extra,
                      bool tasaValida, int fallasNivel, bool nivelInit, bool presionCae) {
  if (fallasNivel >= FALLAS_NIVEL_MAX) return EST_ERROR_SENSOR;
  if (!nivelInit) return EST_OK;

  if (nivel >= NIVEL_DESBORDE_ON || (prev == EST_DESBORDE && nivel >= NIVEL_DESBORDE_OFF))
    return EST_DESBORDE;

  if (tasaValida && tasa < 0 && extra <= UMBRAL_FUGA_EXTRA_PCT_HORA)
    return EST_FUGA;

  float umbralEvap = presionCae ? EVAP_RIESGO_PRES : EVAP_RIESGO;
  if (nivel <= NIVEL_SEQUIA_ON ||
      (prev == EST_SEQUIA && nivel <= NIVEL_SEQUIA_OFF) ||
      (nivel <= NIVEL_RIESGO && evap >= umbralEvap))
    return EST_SEQUIA;

  return EST_OK;
}

void registrarEvento(uint8_t codigo, unsigned long t) {   // con mutexDatos tomado
  eventos[idxEventos] = { t, codigo };
  idxEventos = (idxEventos + 1) % MAX_EVENTOS;
  if (totalEventos < MAX_EVENTOS) totalEventos++;

  // Copia para la tarea MQTT. Si la cola esta llena se descarta el mas antiguo.
  if (colaEventos) {
    Evento e = { t, codigo };
    if (xQueueSend(colaEventos, &e, 0) != pdTRUE) {
      Evento viejo;
      xQueueReceive(colaEventos, &viejo, 0);
      xQueueSend(colaEventos, &e, 0);
    }
  }
}

// Activa/desactiva una falla. Llamar CON mutexDatos tomado.
void aplicarFalla(uint8_t bit, bool activa, unsigned long t) {
  bool actual = (datos.fallas & bit) != 0;
  if (actual == activa) return;
  if (activa) datos.fallas |= bit; else datos.fallas &= ~bit;

  int idx = __builtin_ctz(bit);                 // 0..6
  if (bit != F_ULTRA)                           // el ultrasonico ya genera el evento ERROR_SENSOR
    registrarEvento(activa ? (EV_FALLA_BASE + idx) : (EV_RECUP_BASE + idx), t);
  if (activa && (bit & F_SONORA)) datos.silenciada = false;
  Serial.printf("[falla] %s %s\n", NOMBRE_EVENTO[EV_FALLA_BASE + idx], activa ? "ACTIVA" : "resuelta");
}

// Version que toma el mutex (para llamar desde loop())
void reportarFalla(uint8_t bit, bool activa) {
  if (xSemaphoreTake(mutexDatos, pdMS_TO_TICKS(100)) == pdTRUE) {
    aplicarFalla(bit, activa, millis());
    xSemaphoreGive(mutexDatos);
  }
}

Datos snapshotDatos() {
  static Datos ultimo = {};
  if (xSemaphoreTake(mutexDatos, pdMS_TO_TICKS(50)) == pdTRUE) {
    ultimo = datos;
    xSemaphoreGive(mutexDatos);
  }
  return ultimo;
}

void refrescarOLED() {
  if (!oledOk) return;
  if (xSemaphoreTake(mutexI2C, pdMS_TO_TICKS(150)) == pdTRUE) {
    display.display();
    xSemaphoreGive(mutexI2C);
  }
}

// Comprueba si un dispositivo responde en el bus I2C. Llamar CON mutexI2C tomado.
bool i2cResponde(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// ---------------- TAREA DE SENSADO ----------------
void tareaSensado(void *parametro) {

  float tLocal = 20.0f, hLocal = 50.0f;
  int fallasDHT = 0;

  float presion = 0, tBmp = 0;
  int fallasBmp = 0;
  float presRef = -1;
  unsigned long tPres = 0;
  float presDelta = 0;
  bool presionCae = false;

  int fallasUV = 0;

  float nivelEMA = -1;
  float distValida = -1;
  int fallasNivel = 0;

  float nivelTend = -1;
  unsigned long tTend = 0;
  int8_t tend = 0;

  float nivelTasa = -1;
  unsigned long tTasa = 0;
  float tasaReal = 0;
  bool tasaValida = false;

  for (;;) {
    unsigned long ahora = millis();

    // ---------- DHT22 ----------
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    bool dhtValido = !isnan(t) && !isnan(h) && t > -40.0f && t < 80.0f && h >= 0.0f && h <= 100.0f;
    if (dhtValido) {
      tLocal = CAL_T_A * t + CAL_T_B;
      hLocal = constrain(CAL_H_A * h + CAL_H_B, 0.0f, 100.0f);
      fallasDHT = 0;
    } else if (fallasDHT < 255) {
      fallasDHT++;
    }

    // ---------- BMP180: comprueba que responda y se reinicializa si vuelve ----------
    if (xSemaphoreTake(mutexI2C, pdMS_TO_TICKS(150)) == pdTRUE) {
      bool presente = i2cResponde(BMP_ADDR);
      float p = 0, tb = 0;
      if (presente) {
        if (!bmpIniciado) bmpIniciado = bmp.begin(BMP085_STANDARD, &Wire);
        if (bmpIniciado) {
          p = bmp.readPressure() / 100.0f;
          tb = bmp.readTemperature();
        }
      } else {
        bmpIniciado = false;     // al volver, se reinicializa
      }
      xSemaphoreGive(mutexI2C);

      bool bmpValido = presente && bmpIniciado && p > 300.0f && p < 1100.0f && tb > -40.0f && tb < 85.0f;
      if (bmpValido) {
        presion = CAL_P_A * p + CAL_P_B;
        tBmp = tb;
        fallasBmp = 0;
      } else if (fallasBmp < 255) {
        fallasBmp++;
      }
    }
    bool bmpFalla = (fallasBmp >= FALLAS_SENSOR_MAX);

    if (!bmpFalla && presion > 0) {
      if (presRef < 0) { presRef = presion; tPres = ahora; }
      else if (ahora - tPres >= PRES_VENTANA_MS) {
        presDelta = presion - presRef;
        presionCae = (presDelta <= -PRES_CAIDA_HPA);
        presRef = presion;
        tPres = ahora;
      }
    }
    if (bmpFalla) presionCae = false;

    // ---------- UV ----------
    float uvV = 0;
    bool uvValido = leerUV(uvV);
    if (uvValido) fallasUV = 0;
    else if (fallasUV < 255) fallasUV++;
    bool uvFalla = (fallasUV >= FALLAS_SENSOR_MAX);
    float uvIdx = uvFalla ? 0.0f : max(0.0f, (uvV - UV_V_OFFSET) / UV_V_POR_INDICE);

    // ---------- Nivel (1 cm = 100 %, 10 cm = 0 %) ----------
    float dist = medirDistanciaMediana(tLocal);
    if (dist > 0) {
      fallasNivel = 0;
      distValida = dist;
      float n = (DIST_TANQUE_VACIO - dist) / (DIST_TANQUE_VACIO - DIST_TANQUE_LLENO) * 100.0f;
      n = constrain(n, 0.0f, 100.0f);
      nivelEMA = (nivelEMA < 0) ? n : (NIVEL_EMA_ALFA * n + (1.0f - NIVEL_EMA_ALFA) * nivelEMA);
    } else if (fallasNivel < 255) {
      fallasNivel++;
    }

    if (nivelEMA >= 0) {
      if (nivelTend < 0) { nivelTend = nivelEMA; tTend = ahora; }
      else if (ahora - tTend >= TENDENCIA_MS) {
        float d = nivelEMA - nivelTend;
        tend = (d > 1.0f) ? 1 : ((d < -1.0f) ? -1 : 0);
        nivelTend = nivelEMA;
        tTend = ahora;
      }
      if (nivelTasa < 0) { nivelTasa = nivelEMA; tTasa = ahora; }
      else if (ahora - tTasa >= VENTANA_TASA_MS) {
        float horas = (ahora - tTasa) / 3600000.0f;
        tasaReal = (nivelEMA - nivelTasa) / horas;
        tasaValida = true;
        nivelTasa = nivelEMA;
        tTasa = ahora;
      }
    }

    float evap = calcularIndiceEvaporacion(tLocal, hLocal, uvIdx);
    float esperada = -(evap / 100.0f) * TASA_EVAP_MAX_PCT_HORA;
    float extra = tasaValida ? (tasaReal - esperada) : 0.0f;

    Estado estadoLog = EST_OK;
    uint8_t fallasLog = 0;

    // ---------- Datos compartidos ----------
    if (xSemaphoreTake(mutexDatos, pdMS_TO_TICKS(200)) == pdTRUE) {
      Estado prev = datos.estado;

      aplicarFalla(F_DHT,   fallasDHT   >= FALLAS_SENSOR_MAX, ahora);
      aplicarFalla(F_BMP,   bmpFalla,                         ahora);
      aplicarFalla(F_UV,    uvFalla,                          ahora);
      aplicarFalla(F_ULTRA, fallasNivel >= FALLAS_NIVEL_MAX,  ahora);

      datos.temp = tLocal;
      datos.hum = hLocal;
      datos.presion = presion;
      datos.tempBmp = tBmp;
      datos.presDelta = presDelta;
      datos.presionCae = presionCae;
      datos.uvV = uvV;
      datos.uvIdx = uvIdx;
      datos.catUV = categoriaUV(uvIdx);
      if (nivelEMA >= 0) datos.nivel = nivelEMA;
      datos.distCm = (fallasNivel >= FALLAS_NIVEL_MAX) ? -1.0f : distValida;
      datos.tendencia = tend;
      datos.evap = evap;
      datos.tasaReal = tasaValida ? tasaReal : 0.0f;
      datos.tasaEsperada = esperada;
      datos.perdidaExtra = extra;

      Estado nuevo = calcularEstado(prev, datos.nivel, evap, tasaReal, extra,
                                    tasaValida, fallasNivel, nivelEMA >= 0, presionCae);
      if (nuevo != prev) {
        datos.estado = nuevo;
        datos.silenciada = false;
        registrarEvento((uint8_t)nuevo, ahora);
      }
      estadoLog = datos.estado;
      fallasLog = datos.fallas;

      if (ahora - ultimoRegistroHist >= INTERVALO_HISTORICO_MS) {
        ultimoRegistroHist = ahora;
        historico[idxHistorico] = { ahora, datos.nivel, tLocal, hLocal, uvIdx, evap, presion, datos.estado };
        idxHistorico = (idxHistorico + 1) % TAM_HISTORICO;
        if (totalHistorico < TAM_HISTORICO) totalHistorico++;
      }
      xSemaphoreGive(mutexDatos);
    }

    Serial.printf("[sens] nivel=%.1f%% dist=%.2fcm T=%.1f H=%.0f P=%.1fhPa UVv=%.3f UVi=%.1f evap=%.0f tasa=%.2f extra=%.2f fallas=0x%02X estado=%s\n",
                  nivelEMA, dist, tLocal, hLocal, presion, uvV, uvIdx, evap,
                  tasaReal, extra, fallasLog, NOMBRE_ESTADO[estadoLog]);

    vTaskDelay(pdMS_TO_TICKS(PERIODO_SENSADO_MS));
  }
}

// ---------------- CONTROL DE ACCESO WEB ----------------
bool ipEnSubred(IPAddress ip) {
  if (WiFi.status() != WL_CONNECTED) return false;   // sin WLAN autorizada, nadie pasa
  uint32_t mascara = (uint32_t)WiFi.subnetMask();
  uint32_t local = (uint32_t)WiFi.localIP();
  return ((uint32_t)ip & mascara) == (local & mascara);
}

bool acceso(AsyncWebServerRequest *r) {
  IPAddress ipCliente = r->client()->remoteIP();
  if (!ipEnSubred(ipCliente)) {
    Serial.printf("[web] 403 IP fuera de subred: %s\n", ipCliente.toString().c_str());
    r->send(403, "text/plain", "Acceso restringido a la WLAN autorizada");
    return false;
  }
  if (!r->authenticate(WEB_USER, WEB_PASS)) {
    r->requestAuthentication();
    return false;
  }
  return true;
}

// ---------------- PAGINA WEB ----------------
const char PAGINA_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>AquaMonitor</title>
<link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 130'%3E%3Cpath d='M50 6 C50 6 14 52 14 82 A36 36 0 0 0 86 82 C86 52 50 6 50 6 Z' fill='%23087f8c'/%3E%3C/svg%3E">
<style>
:root{--bg:#f4f8f8;--surface:#fff;--primary:#087f8c;--cyan:#22a6b3;--text:#17313a;--text-secondary:#60777d;--text-light:#8ca0a5;--border:#dce9e9;
--green:#168a68;--green-bg:#e7f7f1;--yellow:#c58216;--yellow-bg:#fff5dc;--red:#c94b4b;--red-bg:#fdecec;--purple:#9156a8;--purple-bg:#f5eafa;--gray:#64748b;--gray-bg:#edf1f3;
--radius:18px;--shadow:0 8px 30px rgba(19,67,75,.07)}
*{box-sizing:border-box}
body{margin:0;padding:28px 20px 50px;font-family:Inter,system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;background:radial-gradient(circle at 90% 0%,rgba(34,166,179,.08),transparent 28%),var(--bg);color:var(--text);line-height:1.5}
.container{max-width:1080px;margin:auto}
header{display:flex;align-items:center;justify-content:space-between;margin-bottom:26px}
.brand{display:flex;align-items:center;gap:13px}
.brand-icon{width:52px;height:52px;display:flex;align-items:center;justify-content:center;border-radius:15px;background:var(--surface);border:1px solid var(--border);box-shadow:0 8px 18px rgba(8,127,140,.12)}
.brand-icon svg{display:block}
.brand-text h1{margin:0;font-size:1.35rem;font-weight:400;letter-spacing:-.3px;color:var(--text)}
.brand-text h1 b{font-weight:800;color:var(--primary)}
.brand-text span{display:block;color:var(--text-light);font-size:.78rem}
.connection{display:flex;align-items:center;gap:8px;padding:8px 13px;border-radius:30px;background:var(--surface);border:1px solid var(--border);color:var(--text-secondary);font-size:.78rem;font-weight:600}
.connection-dot{width:8px;height:8px;border-radius:50%;background:var(--green);box-shadow:0 0 0 4px rgba(22,138,104,.1)}
#estadoBox{padding:16px 20px;border-radius:var(--radius);text-align:center;font-size:.92rem;font-weight:700;margin-bottom:22px;box-shadow:var(--shadow)}
.estado.OK{background:var(--green-bg);color:var(--green);border:1px solid #bfe8d9}
.estado.SEQUIA{background:var(--yellow-bg);color:var(--yellow);border:1px solid #f0dca8}
.estado.DESBORDE{background:var(--red-bg);color:var(--red);border:1px solid #f3c6c6}
.estado.FUGA{background:var(--purple-bg);color:var(--purple);border:1px solid #e3c8eb}
.estado.ERROR_SENSOR{background:var(--gray-bg);color:var(--gray);border:1px solid #d7dfe3}
#fallasBox{display:none;padding:13px 18px;border-radius:14px;margin-bottom:22px;background:var(--yellow-bg);color:var(--yellow);border:1px solid #f0dca8;font-size:.86rem;font-weight:700;text-align:center}
button{width:100%;padding:14px 20px;margin-bottom:22px;border:0;border-radius:14px;background:var(--red);color:#fff;font-size:.9rem;font-weight:700;cursor:pointer;box-shadow:0 7px 18px rgba(201,75,75,.18)}
.hero-card{position:relative;overflow:hidden;background:linear-gradient(135deg,#075e68,#087f8c 55%,#22a6b3);color:#fff;border-radius:24px;padding:34px 25px;text-align:center;margin-bottom:28px;box-shadow:0 18px 45px rgba(8,127,140,.2)}
.hero-card .label{font-size:.78rem;font-weight:700;letter-spacing:1.7px;text-transform:uppercase;color:rgba(255,255,255,.72)}
.hero-card .v{margin:8px 0 10px;font-size:clamp(4rem,11vw,6rem);line-height:1;font-weight:800;letter-spacing:-4px}
#infoExtra{display:inline-block;padding:8px 15px;border-radius:30px;background:rgba(255,255,255,.11);border:1px solid rgba(255,255,255,.15);color:rgba(255,255,255,.85);font-size:.78rem}
h2{display:flex;align-items:center;gap:10px;margin:30px 0 13px;font-size:.78rem;color:var(--text-secondary);font-weight:800;text-transform:uppercase;letter-spacing:1.5px}
h2::before{content:"";width:4px;height:17px;border-radius:5px;background:var(--primary)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:14px;margin-bottom:22px}
.card{min-height:115px;background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:19px;display:flex;flex-direction:column;justify-content:center;box-shadow:var(--shadow)}
.card .label{color:var(--text-light);font-size:.72rem;font-weight:800;text-transform:uppercase;letter-spacing:.8px}
.card .v{margin-top:7px;font-size:1.55rem;line-height:1.2;font-weight:800;letter-spacing:-.5px}
.panel{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);overflow:hidden;margin-bottom:24px;box-shadow:var(--shadow)}
ul{list-style:none;margin:0;padding:0}
li{display:flex;align-items:center;padding:14px 18px;border-bottom:1px solid #edf2f2;color:var(--text-secondary);font-size:.86rem}
li:last-child{border-bottom:none}
li::before{content:"";width:7px;height:7px;flex-shrink:0;margin-right:11px;border-radius:50%;background:var(--primary)}
.table-wrap{overflow-x:auto}
table{width:100%;border-collapse:collapse;text-align:left}
th{padding:13px 18px;background:#f7faf9;color:var(--text-light);font-size:.68rem;text-transform:uppercase;letter-spacing:.8px;font-weight:800;border-bottom:1px solid var(--border)}
td{padding:13px 18px;color:var(--text-secondary);font-size:.82rem;white-space:nowrap;border-bottom:1px solid #edf2f2}
tr:last-child td{border-bottom:none}
@media(max-width:500px){body{padding:18px 13px 35px}.connection span{display:none}.card{min-height:100px;padding:14px}.card .v{font-size:1.25rem}.hero-card .v{font-size:4.2rem}#infoExtra{font-size:.7rem}}
</style>
</head>
<body>
<div class="container">

<header>
  <div class="brand">
    <div class="brand-icon">
      <svg width="30" height="39" viewBox="0 0 100 130" role="img" aria-label="Logo AquaMonitor">
        <defs><clipPath id="gotaLogo"><path d="M50 6 C50 6 14 52 14 82 A36 36 0 0 0 86 82 C86 52 50 6 50 6 Z"/></clipPath></defs>
        <g clip-path="url(#gotaLogo)">
          <path d="M0 74 Q12.5 67 25 74 T50 74 T75 74 T100 74 V130 H0 Z" fill="#22a6b3"/>
          <path d="M0 90 Q12.5 84 25 90 T50 90 T75 90 T100 90 V130 H0 Z" fill="#087f8c"/>
        </g>
        <path d="M50 6 C50 6 14 52 14 82 A36 36 0 0 0 86 82 C86 52 50 6 50 6 Z" fill="none" stroke="#087f8c" stroke-width="7" stroke-linejoin="round"/>
        <line x1="56" y1="42" x2="66" y2="42" stroke="#17313a" stroke-width="5" stroke-linecap="round"/>
        <line x1="56" y1="56" x2="72" y2="56" stroke="#17313a" stroke-width="5" stroke-linecap="round"/>
      </svg>
    </div>
    <div class="brand-text"><h1><b>Aqua</b>Monitor</h1><span>Monitoreo inteligente de agua</span></div>
  </div>
  <div class="connection"><div class="connection-dot"></div><span>Dispositivo conectado</span></div>
</header>

<div id="estadoBox" class="estado OK">Cargando estado...</div>
<div id="fallasBox"></div>
<button id="btnSil" style="display:none" onclick="silenciar()">🔇 Silenciar alarma física</button>

<div class="hero-card">
  <div class="label">Nivel actual del tanque</div>
  <div class="v" id="nivel">--</div>
  <div id="infoExtra">Cargando telemetría...</div>
</div>

<h2>Entorno y clima</h2>
<div class="grid">
  <div class="card"><div class="label">Temperatura</div><div class="v" id="temp">--</div></div>
  <div class="card"><div class="label">Humedad</div><div class="v" id="hum">--</div></div>
  <div class="card"><div class="label">Presión</div><div class="v" id="pres">--</div></div>
  <div class="card"><div class="label">Índice UV</div><div class="v" id="uv">--</div></div>
  <div class="card"><div class="label">Evaporación</div><div class="v" id="evap">--</div></div>
</div>

<h2>Análisis de flujo</h2>
<div class="grid">
  <div class="card"><div class="label">Tasa real</div><div class="v" id="tasa">--</div></div>
  <div class="card"><div class="label">Pérdida esperada</div><div class="v" id="esperada">--</div></div>
  <div class="card"><div class="label">Pérdida extra · fugas</div><div class="v" id="perdida">--</div></div>
</div>

<h2>Gateway y pronóstico (IA)</h2>
<div class="grid">
  <div class="card"><div class="label">Conexión con el Gateway</div><div class="v" id="gw">--</div></div>
  <div class="card"><div class="label">Tiempo a nivel crítico</div><div class="v" id="iaHoras">--</div></div>
  <div class="card"><div class="label">Anomalía</div><div class="v" id="iaAnom">--</div></div>
</div>
<div class="panel"><ul><li id="iaReco">Sin recomendación del Gateway todavía</li></ul></div>

<h2>Registro de eventos</h2>
<div class="panel"><ul id="eventos"></ul></div>

<h2>Histórico reciente</h2>
<div class="panel table-wrap">
<table>
<thead><tr><th>Hora</th><th>Nivel</th><th>Temp</th><th>Hum</th><th>Presión</th><th>UVi</th><th>Evap</th><th>Estado</th></tr></thead>
<tbody id="histBody"></tbody>
</table>
</div>

</div>

<script>
let off = 0;
const $ = id => document.getElementById(id);
const hora = ms => new Date(ms + off).toLocaleTimeString();
const FALLAS = [[1,'DHT22 (temp/humedad)'],[2,'BMP180 (presión)'],[4,'GUVA (UV)'],[8,'Ultrasónico (nivel)'],[16,'OLED'],[32,'WiFi'],[64,'Gateway MQTT']];

function nombreEvento(e){
  if (e === 'OK') return 'Normalizado (OK)';
  if (e.startsWith('FALLA_')) return '⚠ Falla: ' + e.slice(6);
  if (e.startsWith('RECUPERADO_')) return '✔ Recuperado: ' + e.slice(11);
  if (e === 'SILENCIADA_LOCAL') return '🔇 Alarma silenciada desde el tablero local';
  if (e === 'SILENCIADA_NUBE') return '🔇 Alarma silenciada desde la nube';
  return e;
}

async function silenciar(){
  try { await fetch('/silenciar', { method:'POST' }); } catch(e) {}
  actualizar();
}

async function actualizar(){
  try {
    const r = await fetch('/data');
    if(!r.ok) throw new Error(r.status);
    const d = await r.json();
    off = Date.now() - d.up;
    const f = d.fallas;

    $('nivel').textContent = (f & 8) ? 'ERR' : d.nivel.toFixed(1) + '%';
    $('temp').textContent = (f & 1) ? 'ERR' : d.temp.toFixed(1) + '°C';
    $('hum').textContent = (f & 1) ? 'ERR' : d.hum.toFixed(0) + '%';
    $('pres').textContent = (f & 2) ? 'ERR' : d.presion.toFixed(1) + ' hPa';
    $('uv').textContent = (f & 4) ? 'ERR' : d.uvIdx.toFixed(1);
    $('evap').textContent = d.evap.toFixed(0) + '%';
    $('tasa').textContent = d.tasa.toFixed(2) + ' %/h';
    $('esperada').textContent = d.esperada.toFixed(2) + ' %/h';
    $('perdida').textContent = d.perdida.toFixed(2) + ' %/h';

    const box = $('estadoBox');
    box.textContent = 'Estado: ' + d.estado + (d.silenciada ? ' · alarma silenciada' : '');
    box.className = 'estado ' + d.estado;

    const lista = FALLAS.filter(x => f & x[0]).map(x => x[1]);
    const fb = $('fallasBox');
    if (lista.length) { fb.style.display = 'block'; fb.textContent = '⚠ Sensor desconectado o sin lectura: ' + lista.join(', '); }
    else fb.style.display = 'none';

    $('btnSil').style.display = ((d.estado !== 'OK' || (f & 31)) && !d.silenciada) ? 'block' : 'none';
    document.title = ((d.estado !== 'OK' || lista.length) ? '⚠ ' : '') + 'AquaMonitor';
    if ((d.estado !== 'OK' || (f & 31)) && !d.silenciada && navigator.vibrate) navigator.vibrate(200);

    $('infoExtra').textContent =
      (d.dist >= 0 ? 'Dist: ' + d.dist.toFixed(2) + ' cm' : 'Dist: sin eco') +
      ' · UV: ' + d.catUV + ' (' + d.uvV.toFixed(2) + ' V) · Tendencia: ' +
      ['↓ bajando','= estable','↑ subiendo'][d.tendencia + 1] +
      ' · ΔP: ' + d.presDelta.toFixed(1) + ' hPa' + (d.presionCae ? ' ⚠' : '') +
      ' · WiFi: ' + d.rssi + ' dBm';

    $('gw').textContent = (f & 64) ? 'Sin conexión' : 'Conectado';
    if (d.iaValida) {
      $('iaHoras').textContent = d.iaHoras <= -1.5 ? 'Calculando…' : d.iaHoras < 0 ? 'Estable' : (d.iaHoras >= 48 ? (d.iaHoras / 24).toFixed(1) + ' días' : d.iaHoras.toFixed(1) + ' h');
      $('iaAnom').textContent = d.iaAnomalia ? '⚠ Sí' : 'No';
      $('iaReco').textContent = d.iaReco || 'Sin recomendación';
    }

    $('eventos').innerHTML = d.eventos.slice().reverse()
      .map(e => `<li>${hora(e.t)} — ${nombreEvento(e.e)}</li>`).join('')
      || '<li>Sin eventos registrados</li>';

    const h = await (await fetch('/historico')).json();
    $('histBody').innerHTML = h.slice().reverse().map(m => `
      <tr>
        <td>${hora(m.t)}</td>
        <td>${m.nivel.toFixed(1)}%</td>
        <td>${m.temp.toFixed(1)}°C</td>
        <td>${m.hum.toFixed(0)}%</td>
        <td>${m.presion.toFixed(1)}</td>
        <td>${m.uvIdx.toFixed(1)}</td>
        <td>${m.evap.toFixed(0)}%</td>
        <td>${m.estado}</td>
      </tr>`).join('');

  } catch(e) {
    const box = $('estadoBox');
    box.textContent = 'Sin conexión con el dispositivo';
    box.className = 'estado ERROR_SENSOR';
  }
}

setInterval(actualizar, 4000);
actualizar();
</script>
</body>
</html>
)HTML";

// Agrega texto a un JSON escapando comillas, barras y caracteres de control.
void agregarEscapadoJSON(String &j, const char* s) {
  for (; *s; s++) {
    char c = *s;
    if (c == '"' || c == '\\') { j += '\\'; j += c; }
    else if ((uint8_t)c < 0x20) j += ' ';
    else j += c;
  }
}

// ---------------- SERVIDOR WEB ----------------
void configurarServidorWeb() {

  servidorWeb.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!acceso(r)) return;
    r->send_P(200, "text/html", PAGINA_HTML);
  });

  servidorWeb.on("/data", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!acceso(r)) return;

    Datos d;
    Evento ev[MAX_EVENTOS];
    int nEv = 0;

    if (xSemaphoreTake(mutexDatos, pdMS_TO_TICKS(200)) != pdTRUE) {
      r->send(503, "application/json", "{}");
      return;
    }
    d = datos;
    nEv = totalEventos;
    for (int i = 0; i < nEv; i++)
      ev[i] = eventos[(idxEventos - nEv + i + MAX_EVENTOS) % MAX_EVENTOS];
    xSemaphoreGive(mutexDatos);

    String j;
    j.reserve(1600);
    auto num = [&](const char* k, float v, int dec) {
      if (!isfinite(v)) v = 0;
      j += "\""; j += k; j += "\":"; j += String(v, dec); j += ",";
    };

    j += "{\"up\":"; j += String(millis()); j += ",";
    num("nivel", d.nivel, 1);
    num("dist", d.distCm, 2);
    num("temp", d.temp, 1);
    num("hum", d.hum, 1);
    num("presion", d.presion, 1);
    num("tempBmp", d.tempBmp, 1);
    num("presDelta", d.presDelta, 1);
    num("uvV", d.uvV, 3);
    num("uvIdx", d.uvIdx, 2);
    num("evap", d.evap, 1);
    num("tasa", d.tasaReal, 2);
    num("esperada", d.tasaEsperada, 2);
    num("perdida", d.perdidaExtra, 2);
    j += "\"tendencia\":"; j += String((int)d.tendencia); j += ",";
    j += "\"catUV\":\""; j += CAT_UV[d.catUV]; j += "\",";
    j += "\"estado\":\""; j += NOMBRE_ESTADO[d.estado]; j += "\",";
    j += "\"silenciada\":"; j += d.silenciada ? "true" : "false"; j += ",";
    j += "\"presionCae\":"; j += d.presionCae ? "true" : "false"; j += ",";
    j += "\"fallas\":"; j += String((int)d.fallas); j += ",";
    j += "\"rssi\":"; j += String(WiFi.RSSI()); j += ",";
    j += "\"iaValida\":"; j += d.iaValida ? "true" : "false"; j += ",";
    j += "\"iaAnomalia\":"; j += d.iaAnomalia ? "true" : "false"; j += ",";
    num("iaHoras", d.iaHoras, 1);
    j += "\"iaReco\":\""; agregarEscapadoJSON(j, d.iaReco); j += "\",";
    j += "\"eventos\":[";
    for (int i = 0; i < nEv; i++) {
      if (i) j += ",";
      j += "{\"t\":"; j += String(ev[i].t);
      j += ",\"e\":\""; j += NOMBRE_EVENTO[ev[i].codigo]; j += "\"}";
    }
    j += "]}";

    r->send(200, "application/json", j);
  });

  servidorWeb.on("/historico", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!acceso(r)) return;

    if (xSemaphoreTake(mutexDatos, pdMS_TO_TICKS(200)) != pdTRUE) {
      r->send(503, "application/json", "[]");
      return;
    }

    String j;
    j.reserve(150 * totalHistorico + 4);
    j += "[";
    for (int i = 0; i < totalHistorico; i++) {
      int idx = (idxHistorico - totalHistorico + i + TAM_HISTORICO) % TAM_HISTORICO;
      const Muestra &m = historico[idx];
      char buf[220];
      snprintf(buf, sizeof(buf),
               "%s{\"t\":%lu,\"nivel\":%.1f,\"temp\":%.1f,\"hum\":%.1f,\"presion\":%.1f,\"uvIdx\":%.2f,\"evap\":%.1f,\"estado\":\"%s\"}",
               i ? "," : "", m.t, m.nivel, m.temp, m.hum, m.presion, m.uvIdx, m.evap,
               NOMBRE_ESTADO[m.estado]);
      j += buf;
    }
    xSemaphoreGive(mutexDatos);
    j += "]";
    r->send(200, "application/json", j);
  });

  servidorWeb.on("/silenciar", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!acceso(r)) return;
    if (xSemaphoreTake(mutexDatos, pdMS_TO_TICKS(200)) == pdTRUE) {
      if ((datos.estado != EST_OK || (datos.fallas & F_SONORA)) && !datos.silenciada) {
        datos.silenciada = true;
        registrarEvento(EV_SILENCIO_LOCAL, millis());
      }
      xSemaphoreGive(mutexDatos);
      Serial.println("[web] alarma silenciada desde el tablero");
      r->send(200, "application/json", "{\"ok\":true}");
    } else {
      r->send(503, "application/json", "{\"ok\":false}");
    }
  });

  servidorWeb.onNotFound([](AsyncWebServerRequest *r) {
    r->send(404, "text/plain", "No encontrado");
  });
}


// ---------------- MQTT: TAREA DE TRANSMISION ----------------
// Corre en su propia tarea FreeRTOS (nucleo 0). Es la unica que usa el
// cliente MQTT, asi que PubSubClient no necesita mutex propio.
WiFiClient clienteTCP;
PubSubClient mqtt(clienteTCP);

char topicTelemetria[64], topicEventos[64], topicEstado[64], topicCmd[64], topicIA[64];

void armarTopics() {
  snprintf(topicTelemetria, sizeof(topicTelemetria), "aquamonitor/%s/telemetria", ESTACION_ID);
  snprintf(topicEventos,    sizeof(topicEventos),    "aquamonitor/%s/eventos",    ESTACION_ID);
  snprintf(topicEstado,     sizeof(topicEstado),     "aquamonitor/%s/estado",     ESTACION_ID);
  snprintf(topicCmd,        sizeof(topicCmd),        "aquamonitor/%s/cmd",        ESTACION_ID);
  snprintf(topicIA,         sizeof(topicIA),         "aquamonitor/%s/ia",         ESTACION_ID);
}

// Busca "clave":numero en un JSON plano (suficiente para los mensajes del Gateway).
bool jsonNumero(const char* json, const char* clave, float &out) {
  char patron[40];
  snprintf(patron, sizeof(patron), "\"%s\":", clave);
  const char* p = strstr(json, patron);
  if (!p) return false;
  out = atof(p + strlen(patron));
  return true;
}

// Busca "clave":"texto" y copia el texto (deshace \" y \\).
bool jsonTexto(const char* json, const char* clave, char* out, size_t n) {
  char patron[40];
  snprintf(patron, sizeof(patron), "\"%s\":\"", clave);
  const char* p = strstr(json, patron);
  if (!p) return false;
  p += strlen(patron);
  size_t i = 0;
  while (*p && *p != '"' && i < n - 1) {
    if (*p == '\\' && *(p + 1)) p++;
    out[i++] = *p++;
  }
  out[i] = 0;
  return true;
}

// Mensajes que llegan del Gateway: ordenes y resultados de la IA.
void alRecibirMQTT(char* topic, byte* payload, unsigned int len) {
  static char buf[512];
  unsigned int n = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
  memcpy(buf, payload, n);
  buf[n] = 0;

  if (strcmp(topic, topicCmd) == 0) {
    if (strstr(buf, "silenciar")) {
      bool aplicada = false;
      if (xSemaphoreTake(mutexDatos, pdMS_TO_TICKS(200)) == pdTRUE) {
        if ((datos.estado != EST_OK || (datos.fallas & F_SONORA)) && !datos.silenciada) {
          datos.silenciada = true;
          registrarEvento(EV_SILENCIO_NUBE, millis());
          aplicada = true;
        }
        xSemaphoreGive(mutexDatos);
      }
      Serial.printf("[mqtt] orden silenciar recibida (%s)\n", aplicada ? "aplicada" : "sin alarma activa");
    }
  } else if (strcmp(topic, topicIA) == 0) {
    float horas = -1, anom = 0;
    char reco[200] = "";
    jsonNumero(buf, "horas_critico", horas);
    jsonNumero(buf, "anomalia", anom);
    jsonTexto(buf, "recomendacion", reco, sizeof(reco));
    if (xSemaphoreTake(mutexDatos, pdMS_TO_TICKS(200)) == pdTRUE) {
      datos.iaValida = true;
      datos.iaHoras = horas;
      datos.iaAnomalia = anom > 0.5f;
      strncpy(datos.iaReco, reco, sizeof(datos.iaReco) - 1);
      datos.iaReco[sizeof(datos.iaReco) - 1] = 0;
      xSemaphoreGive(mutexDatos);
    }
    Serial.printf("[mqtt] IA: horas=%.1f anomalia=%d\n", horas, anom > 0.5f);
  }
}

// Obtiene la IP del broker. Acepta una IP ("192.168.1.10") o un nombre
// ".local" que se resuelve por mDNS en cada intento: asi el ESP32 encuentra
// la Raspberry aunque el hotspot o el router le cambien la IP.
bool resolverBroker(IPAddress &ip) {
  if (ip.fromString(MQTT_HOST)) return true;
  size_t n = strlen(MQTT_HOST);
  if (n > 6 && strcmp(MQTT_HOST + n - 6, ".local") == 0) {
    static bool mdnsIniciado = false;
    if (!mdnsIniciado) mdnsIniciado = MDNS.begin("aquamonitor-esp32");
    String nombre = String(MQTT_HOST).substring(0, n - 6);
    IPAddress r = MDNS.queryHost(nombre, 2000);
    if ((uint32_t)r == 0) return false;
    ip = r;
    return true;
  }
  return WiFi.hostByName(MQTT_HOST, ip) == 1;      // nombre DNS normal
}

bool conectarMQTT() {
  IPAddress ipBroker;
  if (!resolverBroker(ipBroker)) {
    Serial.printf("[mqtt] no se encontro %s en la red\n", MQTT_HOST);
    return false;
  }
  mqtt.setServer(ipBroker, MQTT_PORT);
  char idCliente[40];
  snprintf(idCliente, sizeof(idCliente), "aquamonitor-%s-%04X", ESTACION_ID, (uint16_t)(ESP.getEfuseMac() & 0xFFFF));
  // LWT: si el ESP32 se desconecta sin avisar, el broker publica "offline".
  bool ok = mqtt.connect(idCliente, MQTT_USER, MQTT_PASS, topicEstado, 1, true, "offline");
  if (ok) {
    mqtt.publish(topicEstado, "online", true);
    mqtt.subscribe(topicCmd, 1);
    mqtt.subscribe(topicIA, 1);
    Serial.printf("[mqtt] conectado a %s (%s):%u como %s\n", MQTT_HOST,
                  ipBroker.toString().c_str(), MQTT_PORT, MQTT_USER);
  } else {
    Serial.printf("[mqtt] fallo de conexion (rc=%d)\n", mqtt.state());
  }
  return ok;
}

bool publicarTelemetria() {
  Datos d;
  if (xSemaphoreTake(mutexDatos, pdMS_TO_TICKS(200)) != pdTRUE) return false;
  d = datos;
  xSemaphoreGive(mutexDatos);

  char j[640];
  snprintf(j, sizeof(j),
    "{\"est\":\"%s\",\"seq\":%lu,\"up\":%lu,\"nivel\":%.1f,\"dist\":%.2f,\"temp\":%.1f,\"hum\":%.1f,"
    "\"presion\":%.1f,\"presDelta\":%.1f,\"presionCae\":%d,\"uvIdx\":%.2f,\"evap\":%.1f,"
    "\"tasa\":%.2f,\"esperada\":%.2f,\"perdida\":%.2f,\"tendencia\":%d,"
    "\"estado\":\"%s\",\"estadoCod\":%d,\"fallas\":%d,\"silenciada\":%d,\"rssi\":%d}",
    ESTACION_ID, (unsigned long)seqTelemetria, millis(),
    isfinite(d.nivel) ? d.nivel : 0, d.distCm, d.temp, d.hum,
    d.presion, d.presDelta, d.presionCae ? 1 : 0, d.uvIdx, d.evap,
    d.tasaReal, d.tasaEsperada, d.perdidaExtra, (int)d.tendencia,
    NOMBRE_ESTADO[d.estado], (int)d.estado, (int)d.fallas, d.silenciada ? 1 : 0, (int)WiFi.RSSI());
  bool ok = mqtt.publish(topicTelemetria, j);
  if (ok) seqTelemetria++;
  return ok;
}

// Publica los eventos pendientes; solo los saca de la cola si se enviaron.
void publicarEventosPendientes() {
  Evento e;
  while (xQueuePeek(colaEventos, &e, 0) == pdTRUE) {
    char j[160];
    snprintf(j, sizeof(j), "{\"est\":\"%s\",\"up\":%lu,\"t\":%lu,\"evento\":\"%s\",\"codigo\":%d}",
             ESTACION_ID, millis(), e.t, NOMBRE_EVENTO[e.codigo], (int)e.codigo);
    if (!mqtt.publish(topicEventos, j)) break;
    xQueueReceive(colaEventos, &e, 0);
  }
}

void tareaMQTT(void *parametro) {
  armarTopics();
  mqtt.setBufferSize(MQTT_BUFFER_BYTES);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(3);
  mqtt.setCallback(alRecibirMQTT);

  unsigned long ultimoIntento = 0, ultimaTelemetria = 0;
  bool conectadoAntes = false;

  for (;;) {
    unsigned long ahora = millis();
    bool conectado = mqtt.connected();

    if (!conectado && WiFi.status() == WL_CONNECTED && ahora - ultimoIntento >= MQTT_RETRY_MS) {
      ultimoIntento = ahora;
      conectado = conectarMQTT();
    }
    if (conectado != conectadoAntes) {          // cambio de estado de la conexion
      reportarFalla(F_MQTT, !conectado);
      conectadoAntes = conectado;
    }

    if (conectado) {
      mqtt.loop();                              // procesa entrantes y keep-alive
      publicarEventosPendientes();
      if (ahora - ultimaTelemetria >= PERIODO_TELEMETRIA_MS) {
        if (publicarTelemetria()) ultimaTelemetria = ahora;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ---------------- ICONOS OLED ----------------
void iconoTermometro(int x, int y) {
  display.drawRect(x + 1, y, 5, 9, SH110X_WHITE);
  display.fillCircle(x + 3, y + 11, 3, SH110X_WHITE);
}

void iconoGota(int x, int y) {
  display.fillTriangle(x + 3, y, x, y + 6, x + 6, y + 6, SH110X_WHITE);
  display.fillCircle(x + 3, y + 7, 3, SH110X_WHITE);
}

void iconoSol(int x, int y) {
  display.drawCircle(x + 4, y + 6, 3, SH110X_WHITE);
  display.drawLine(x + 4, y - 1, x + 4, y + 1, SH110X_WHITE);
  display.drawLine(x + 4, y + 11, x + 4, y + 13, SH110X_WHITE);
  display.drawLine(x - 2, y + 6, x, y + 6, SH110X_WHITE);
  display.drawLine(x + 8, y + 6, x + 10, y + 6, SH110X_WHITE);
}

void iconoTendencia(int8_t tendencia, int x, int y) {
  if (tendencia == 1) {
    display.fillTriangle(x + 7, y, x, y + 9, x + 14, y + 9, SH110X_WHITE);
  } else if (tendencia == -1) {
    display.fillTriangle(x, y, x + 14, y, x + 7, y + 9, SH110X_WHITE);
  } else {
    display.drawLine(x, y + 3, x + 14, y + 3, SH110X_WHITE);
    display.drawLine(x, y + 6, x + 14, y + 6, SH110X_WHITE);
  }
}

// ---------------- OLED: MENSAJES, INICIO, PANTALLA ----------------
void mostrarMensaje(const char* l1, const String& l2) {
  Serial.printf("[oled] %s | %s\n", l1, l2.c_str());
  if (!oledOk) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(4, 16); display.print(l1);
  display.setCursor(4, 34); display.print(l2);
  refrescarOLED();
}

void pantallaInicio() {
  if (!oledOk) return;

  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(2);
  display.setCursor(10, 10); display.println("AQUA");
  display.setCursor(10, 30); display.println("MONITOR");
  refrescarOLED();
  delay(1000);

  for (int i = 0; i <= 100; i += 10) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(15, 20);
    display.println("Iniciando sistema...");
    display.drawRect(14, 35, 100, 10, SH110X_WHITE);
    display.fillRect(16, 37, map(i, 0, 100, 0, 96), 6, SH110X_WHITE);
    display.setCursor(50, 50);
    display.print(i); display.print("%");
    refrescarOLED();
    delay(30);
  }
}

void textoFallasOLED(uint8_t f, char* out, size_t n) {
  String s = "FALLA:";
  if (f & F_DHT) s += " DHT";
  if (f & F_BMP) s += " BMP";
  if (f & F_UV)  s += " UV";
  s.toCharArray(out, n);
}

void dibujarPantalla(const Datos& d) {

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  bool alarma = (d.estado != EST_OK);
  bool falla  = (d.fallas & F_EN_OLED) != 0;
  bool invertir = alarma && (d.silenciada || buzzerEncendido);

  if (invertir) {
    display.fillRect(0, 0, 128, 11, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    const char* txt = TEXTO_ALARMA[d.estado];
    display.setCursor((128 - strlen(txt) * 6) / 2, 2);
    display.print(txt);
    display.setTextColor(SH110X_WHITE);
  } else if (!alarma && falla) {
    char txt[24];
    textoFallasOLED(d.fallas, txt, sizeof(txt));
    display.fillRect(0, 0, 128, 11, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    display.setCursor((128 - strlen(txt) * 6) / 2, 2);
    display.print(txt);
    display.setTextColor(SH110X_WHITE);
  } else {
    display.setCursor(2, 2);
    display.print("MONITOR DE AGUA");
    display.setCursor(100, 2);
    display.print(WiFi.status() == WL_CONNECTED ? "W" : "x");
    display.setCursor(108, 2);
    display.print((d.fallas & F_MQTT) ? "-" : "G");      // G = Gateway conectado
    if (puntoVivoVisible) display.fillCircle(122, 5, 2, SH110X_WHITE);
    else display.drawCircle(122, 5, 2, SH110X_WHITE);
  }

  display.drawLine(0, 12, 128, 12, SH110X_WHITE);

  // Barra del tanque
  const int barraX = 6, barraY = 16, barraAncho = 16, barraAlto = 34;
  display.drawRect(barraX, barraY, barraAncho, barraAlto, SH110X_WHITE);
  int relleno = (int)(d.nivel / 100.0f * (barraAlto - 2));
  int yRelleno = barraY + barraAlto - 1 - relleno;
  int offset = (frameAnimacion % 2 == 0) ? 0 : 1;
  for (int y = yRelleno; y < barraY + barraAlto - 1; y++)
    for (int x = barraX + 1 + offset; x < barraX + barraAncho - 1; x += 2)
      display.drawPixel(x, y, SH110X_WHITE);

  display.setCursor(4, 53);
  if (!(d.fallas & F_ULTRA)) { display.print((int)d.nivel); display.print("%"); }
  else display.print("ERR");

  display.drawLine(28, 15, 28, 61, SH110X_WHITE);

  // Temperatura / Humedad / UV
  iconoTermometro(32, 16);
  display.setCursor(48, 19);
  if (d.fallas & F_DHT) display.print("ERR");
  else { display.print(d.temp, 1); display.print("C"); }

  iconoGota(32, 32);
  display.setCursor(48, 35);
  if (d.fallas & F_DHT) display.print("ERR");
  else { display.print(d.hum, 0); display.print("%"); }

  iconoSol(32, 48);
  display.setCursor(48, 51);
  display.print("UV ");
  if (d.fallas & F_UV) display.print("ERR");
  else display.print(d.uvIdx, 1);

  // Panel derecho
  display.drawLine(90, 15, 90, 61, SH110X_WHITE);
  display.setCursor(92, 16);
  display.print("NIVEL");
  iconoTendencia(d.tendencia, 99, 25);

  display.setCursor(92, 37);
  display.print("E:"); display.print((int)d.evap); display.print("%");

  // Alterna cada ~1 s: categoria UV <-> presion
  display.setCursor(92, 47);
  if (frameAnimacion < 2) {
    if (d.fallas & F_UV) display.print("ERR");
    else display.print(CAT_UV_CORTA[d.catUV]);
  } else {
    if (d.fallas & F_BMP) display.print("P:ERR");
    else { display.print((int)d.presion); display.print(d.presionCae ? "!" : "h"); }
  }

  int sev = (d.fallas & F_UV) ? 0 : d.catUV + 1;
  if (sev > 4) sev = 4;
  for (int i = 0; i < 4; i++) {
    int bx = 94 + (i * 8);
    if (i < sev) display.fillRect(bx, 58, 6, 4, SH110X_WHITE);
    else display.drawRect(bx, 58, 6, 4, SH110X_WHITE);
  }

  refrescarOLED();
}

// ---------------- WIFI ----------------
bool conectarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long t0 = millis();
  int puntos = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
    delay(300);
    Serial.print(".");
    String pts = "";
    for (int i = 0; i < (puntos % 4); i++) pts += ".";
    mostrarMensaje("Conectando WiFi", pts);
    puntos++;
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

void gestionarWiFi(unsigned long ahora) {
  static unsigned long ultimoIntento = 0;
  static bool estabaConectado = false;
  static bool fallaReportada = false;

  bool conectado = (WiFi.status() == WL_CONNECTED);
  if (conectado && !estabaConectado) {
    Serial.print("[wifi] conectado. IP: ");
    Serial.println(WiFi.localIP());
  }
  if (!conectado && estabaConectado) Serial.println("[wifi] conexion perdida");
  estabaConectado = conectado;

  if (fallaReportada == conectado) {            // el estado cambio
    fallaReportada = !conectado;
    reportarFalla(F_WIFI, fallaReportada);
  }

  if (!conectado && ahora - ultimoIntento >= WIFI_RETRY_MS) {
    ultimoIntento = ahora;
    Serial.println("[wifi] reintentando...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

// Detecta OLED desconectada y la reinicializa cuando vuelve
void vigilarOLED(unsigned long ahora) {
  static unsigned long ultimo = 0;
  if (ahora - ultimo < CHEQUEO_OLED_MS) return;
  ultimo = ahora;

  if (xSemaphoreTake(mutexI2C, pdMS_TO_TICKS(150)) != pdTRUE) return;
  bool presente = i2cResponde(OLED_ADDR);
  if (!presente) {
    oledOk = false;
  } else if (!oledOk) {
    oledOk = display.begin(OLED_ADDR, true);
  }
  xSemaphoreGive(mutexI2C);

  reportarFalla(F_OLED, !oledOk);
}

// ---------------- SETUP ----------------
void setup() {

  Serial.begin(115200);

  mutexI2C = xSemaphoreCreateMutex();
  mutexDatos = xSemaphoreCreateMutex();
  colaEventos = xQueueCreate(16, sizeof(Evento));
  datos.iaHoras = -1;
  if (mutexI2C == NULL || mutexDatos == NULL || colaEventos == NULL) {
    Serial.println("ERROR: no se pudo crear un mutex o la cola de eventos");
    while (true) delay(1000);
  }

  dht.begin();
  pinMode(GUVA_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(GUVA_PIN, ADC_2_5db);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // ---------- Bus I2C compartido: OLED (0x3C) + BMP180 (0x77) ----------
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(100);         // evita bloqueos si un dispositivo se desconecta

  oledOk = display.begin(OLED_ADDR, true);
  if (!oledOk) Serial.println("ERROR: OLED no encontrado (continuo sin pantalla)");

  bmpIniciado = bmp.begin(BMP085_STANDARD, &Wire);
  Serial.println(bmpIniciado ? "BMP180 OK" : "ERROR: BMP180 no encontrado (se reintenta solo)");

  pantallaInicio();

  xTaskCreatePinnedToCore(tareaSensado, "Sensado", 8192, NULL, 1, NULL, 1);
  // Transmision MQTT en su propio hilo (nucleo 0, junto a la pila WiFi).
  xTaskCreatePinnedToCore(tareaMQTT, "MQTT", 6144, NULL, 1, NULL, 0);

  configurarServidorWeb();
  bool ok = conectarWiFi();
  servidorWeb.begin();

  if (ok) {
    mostrarMensaje("WiFi OK", WiFi.localIP().toString());
    Serial.print("Conectado. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    mostrarMensaje("WiFi FALLO", "codigo " + String((int)WiFi.status()));
    Serial.printf("No se pudo conectar (status=%d). Sigo en modo local y reintento.\n",
                  (int)WiFi.status());
  }
  delay(1500);
  Serial.println("Servidor web iniciado.");
}

// ---------------- LOOP ----------------
void loop() {

  unsigned long ahora = millis();
  Datos d = snapshotDatos();

  // ---------- Buzzer ----------
  // Alarma de estado: pitido continuo alterno (400 ms).
  // Solo falla de sensor: chirrido corto cada 3 s.
  bool alarmaEstado = (d.estado != EST_OK);
  bool fallaSonora  = (d.fallas & F_SONORA) != 0;

  if ((alarmaEstado || fallaSonora) && !d.silenciada) {
    if (alarmaEstado) {
      if (ahora - ultimoBeep >= 400) {
        ultimoBeep = ahora;
        buzzerEncendido = !buzzerEncendido;
        digitalWrite(BUZZER_PIN, buzzerEncendido ? HIGH : LOW);
      }
    } else {
      buzzerEncendido = ((ahora % 3000UL) < 120UL);
      digitalWrite(BUZZER_PIN, buzzerEncendido ? HIGH : LOW);
    }
  } else {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerEncendido = false;
  }

  if (ahora - ultimoFrame >= 500) {
    ultimoFrame = ahora;
    frameAnimacion = (frameAnimacion + 1) % 4;
    puntoVivoVisible = !puntoVivoVisible;
  }

  gestionarWiFi(ahora);
  vigilarOLED(ahora);

  if (oledOk) dibujarPantalla(d);

  delay(100);
}
