// === Librerías ===
#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>

// === WiFi y MQTT ===
const char* ssid = "TIGO-D470";
const char* password = "4D9667306294";
const char* mqtt_server = "galiot.galileo.edu";
const char* user = "monair";
const char* passwd = "MONair2023";
WiFiClient espClient;
PubSubClient client(espClient);

// === Telegram ===
#define BOT_TOKEN "8045777002:AAG_-TFHg6MrGfDvj6OcP6JHlFaLqSuaTNU"
#define CHAT_ID "7189563602"
WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

// === Pines ===
#define A_OUT_GAS_PIN     32
#define D_OUT_GAS_PIN     25
#define A_OUT_LLAMA_PIN   33
#define D_OUT_LLAMA_PIN   13
#define ALERT_BUZZER_PIN  16
#define BME_SDA           21
#define BME_SCL           22

// === Variables globales ===
Adafruit_BME680 bme;
bool bmeOk = false;
float tempBME = 0.0, humBME = 0.0, presBME = 0.0, altBME = 0.0;
int gasAnalogValue = 0, llamaAnalogValue = 0;
float gasPercentage = 0.0;
int lastGasDigitalState = LOW;
int lastLlamaDigitalState = LOW;

bool fuegoDetectado = false;
bool gasDetectado = false;
unsigned long tiempoUltimoEnvioFuego = 0;
unsigned long tiempoUltimoEnvioAmbiente = 0;

// === Función melodía ===
void playMelodiaConexion() {
  int melody[] = {262, 330, 392, 523, 392, 523, 659, 784};
  int duration[] = {200, 200, 200, 200, 200, 150, 150, 400};
  for (int i = 0; i < 8; i++) {
    tone(ALERT_BUZZER_PIN, melody[i]);
    delay(duration[i]);
    noTone(ALERT_BUZZER_PIN);
    delay(50);
  }
}

// === Pines y conexiones ===
void setupOutputs() {
  pinMode(ALERT_BUZZER_PIN, OUTPUT);
  pinMode(A_OUT_GAS_PIN, INPUT);
  pinMode(D_OUT_GAS_PIN, INPUT);
  pinMode(A_OUT_LLAMA_PIN, INPUT);
  pinMode(D_OUT_LLAMA_PIN, INPUT);
}

void setupWiFi() {
  Serial.println("Conectando a WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\nWiFi conectado. IP: " + WiFi.localIP().toString());
  playMelodiaConexion();
}

void reconnectMQTT() {
  while (!client.connected()) {
    Serial.print("Conectando a MQTT...");
    if (client.connect("S.A.F.E", user, passwd)) {
      Serial.println("Conectado");
    } else {
      Serial.print(" fallo, rc="); Serial.println(client.state());
      delay(1000);
    }
  }
}

// === BME680 ===
bool setupBME680() {
  Wire.begin(BME_SDA, BME_SCL);
  if (!bme.begin(0x76, &Wire)) {
    Serial.println("No se encontró el BME680");
    bot.sendMessage(CHAT_ID, "❌ BME680 no detectado en I2C", "");
    return false;
  }
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150);
  return true;
}

bool readBME680() {
  if (!bme.performReading()) return false;
  tempBME = bme.temperature;
  humBME = bme.humidity;
  presBME = bme.pressure / 100.0;
  altBME = 44330.0 * (1.0 - pow(presBME / 1013.25, 1.0 / 5.255));
  return true;
}

// === Lectura de sensores ===
void readGasMQ2() {
  gasAnalogValue = analogRead(A_OUT_GAS_PIN);
  gasPercentage = map(gasAnalogValue, 0, 4095, 0, 100);
  gasPercentage = constrain(gasPercentage, 0, 100);
}

void readLlamaSensor() {
  llamaAnalogValue = analogRead(A_OUT_LLAMA_PIN);
}

void controlOutputs() {
  int gasState = digitalRead(D_OUT_GAS_PIN);
  int llamaState = digitalRead(D_OUT_LLAMA_PIN);
  digitalWrite(ALERT_BUZZER_PIN, (gasState == LOW || llamaState == LOW) ? HIGH : LOW);
}

// === Envío de datos por Telegram ===
void enviarDatosAmbiente() {
  if (!bmeOk || !bme.performReading()) {
    Serial.println("⚠️ Fallo al leer el BME680.");
    return;
  }

  String mensaje = "📡 *Datos del ambiente:*\n";
  mensaje += "🌡️ *Temperatura:* " + String(bme.temperature, 2) + " °C\n";
  mensaje += "💧 *Humedad:* " + String(bme.humidity, 2) + " %\n";
  mensaje += "📈 *Presión:* " + String(bme.pressure / 100.0, 2) + " hPa\n";
  mensaje += "🔥 *Gas BME680:* " + String(bme.gas_resistance / 1000.0, 2) + " KΩ\n";
  mensaje += "💨 *MQ2 Analógico:* " + String(gasAnalogValue) + " / 4095\n";
  mensaje += fuegoDetectado ? "🚨 *Fuego activo.*\n" : "✅ *Sin fuego detectado.*\n";
  mensaje += gasDetectado ? "🚨 *Gas detectado (digital).*" : "✅ *Sin gas detectado.*";

  if (bot.sendMessage(CHAT_ID, mensaje, "Markdown")) {
    Serial.println("✅ Datos enviados por Telegram.");
  } else {
    Serial.println("❌ Error al enviar datos.");
  }
}

// === Envío de datos por MQTT ===
void publishMQTT() {
  char buffer[50];

  snprintf(buffer, sizeof(buffer), "%d", gasAnalogValue);
  client.publish("S.A.F.E/gas_raw", buffer);

  snprintf(buffer, sizeof(buffer), "%.2f", gasPercentage);
  client.publish("S.A.F.E/gas_percentage", buffer);

  snprintf(buffer, sizeof(buffer), "%d", llamaAnalogValue);
  client.publish("S.A.F.E/llama_raw", buffer);

  if (bmeOk) {
    snprintf(buffer, sizeof(buffer), "%.2f", tempBME);
    client.publish("S.A.F.E/bme680/temperature", buffer);
    snprintf(buffer, sizeof(buffer), "%.2f", humBME);
    client.publish("S.A.F.E/bme680/humidity", buffer);
    snprintf(buffer, sizeof(buffer), "%.2f", presBME);
    client.publish("S.A.F.E/bme680/pressure", buffer);
    snprintf(buffer, sizeof(buffer), "%.2f", altBME);
    client.publish("S.A.F.E/bme680/altitude", buffer);
  }
}

// === Setup ===
void setup() {
  Serial.begin(115200);
  setupOutputs();
  setupWiFi();
  secured_client.setInsecure();
  client.setServer(mqtt_server, 1883);

  bot.sendMessage(CHAT_ID, "🤖 *S.A.F.E conectado y monitoreando*", "Markdown");
  bmeOk = setupBME680();

  if (bmeOk) {
    enviarDatosAmbiente();
    tiempoUltimoEnvioAmbiente = millis();
  }
}

// === Loop ===
void loop() {
  if (!client.connected()) reconnectMQTT();
  client.loop();

  if (bmeOk) readBME680();
  readGasMQ2();
  readLlamaSensor();
  controlOutputs();

  // Detectar fuego
  int lecturaLlama = llamaAnalogValue;
  if (lecturaLlama < 400 && !fuegoDetectado) {
    fuegoDetectado = true;
    bot.sendMessage(CHAT_ID, "🔥 *¡FUEGO DETECTADO!*", "Markdown");
    client.publish("S.A.F.E/llama_alert", "🔥 ¡Llama detectada!");
    tiempoUltimoEnvioFuego = millis();
  } else if (fuegoDetectado && millis() - tiempoUltimoEnvioFuego > 10000 && lecturaLlama >= 400) {
    fuegoDetectado = false;
    bot.sendMessage(CHAT_ID, "✅ *Fuego extinguido o fuera de rango.*", "Markdown");
    client.publish("S.A.F.E/llama_alert", "✅ Llama ausente");
  }

  // Detectar gas
  int estadoMQ2 = digitalRead(D_OUT_GAS_PIN);
  if (estadoMQ2 == LOW && !gasDetectado) {
    gasDetectado = true;
    bot.sendMessage(CHAT_ID, "💨 *¡GAS DETECTADO!* (por sensor MQ2 digital)", "Markdown");
    client.publish("S.A.F.E/gas_alert", "⚠️ ¡Gas detectado!");
  } else if (estadoMQ2 == HIGH && gasDetectado) {
    gasDetectado = false;
    bot.sendMessage(CHAT_ID, "✅ *Gas disipado o fuera de rango.*", "Markdown");
    client.publish("S.A.F.E/gas_alert", "✅ Gas normal");
  }

  // Reporte periódico
  if (millis() - tiempoUltimoEnvioAmbiente >= 60000) {
    enviarDatosAmbiente();
    tiempoUltimoEnvioAmbiente = millis();
  }

  // Publicar datos por MQTT
  publishMQTT();

  delay(500);
}
