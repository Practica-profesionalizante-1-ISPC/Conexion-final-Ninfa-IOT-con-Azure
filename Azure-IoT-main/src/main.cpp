#include <Arduino.h>

// C99 libraries
#include <cstdlib>
#include <string.h>
#include <time.h>

// Libraries for MQTT client and WiFi connection
#include <WiFi.h>
#include <mqtt_client.h>

// Azure IoT SDK for C includes
#include <az_core.h>
#include <az_iot.h>
#include <azure_ca.h>

// Additional sample headers 
#include "AzIoTSasToken.h"
#include "SerialLogger.h"
#include "WiFiClientSecure.h"

#include <DHT.h>          // Librería para sensor de TyH
#include <Adafruit_Sensor.h>


//Iniciamos variable de sensor.
int sensorRuido = 34;    // Pin del microfono
int Led = 13;            // Pin led
#define sensorDHT 14     // Pin del sensor DHT
DHT dht(sensorDHT, DHT22);

int ruido = 0;
int hum = 0;
int tem = 0;
int ruidoCorregido2 = 0;
float bTemperatura = 0.02; // Coeficiente de la temperatura
float cHumedad = -0.01;    // Coeficiente de la humedad

// Wifi
#define IOT_CONFIG_WIFI_SSID "TITAN"
#define IOT_CONFIG_WIFI_PASSWORD "info2021docta"

// Azure IoT
#define IOT_CONFIG_IOTHUB_FQDN "ISPC1.azure-devices.net"
#define IOT_CONFIG_DEVICE_ID "dispositivo2"
#define IOT_CONFIG_DEVICE_KEY "btPHnfPNuM7c/gNUrbO30xAxfW/k4IfIPKFOv7I6lqY="

// Tiempo que se envia cada mensaje al server.
#define TELEMETRY_FREQUENCY_MILLISECS 5000

// When developing for your own Arduino-based platform,
// please follow the format '(ard;<platform>)'. 
#define AZURE_SDK_CLIENT_USER_AGENT "c/" AZ_SDK_VERSION_STRING "(ard;esp32)"

// Utility macros and defines
#define sizeofarray(a) (sizeof(a) / sizeof(a[0]))
#define NTP_SERVERS "pool.ntp.org", "time.nist.gov"
#define MQTT_QOS1 1
#define DO_NOT_RETAIN_MSG 0
#define SAS_TOKEN_DURATION_IN_MINUTES 60
#define UNIX_TIME_NOV_13_2017 1510592825

#define PST_TIME_ZONE -8
#define PST_TIME_ZONE_DAYLIGHT_SAVINGS_DIFF   1

#define GMT_OFFSET_SECS (PST_TIME_ZONE * 3600)
#define GMT_OFFSET_SECS_DST ((PST_TIME_ZONE + PST_TIME_ZONE_DAYLIGHT_SAVINGS_DIFF) * 3600)

// Translate iot_configs.h defines into variables used by the sample
static const char* ssid = IOT_CONFIG_WIFI_SSID;
static const char* password = IOT_CONFIG_WIFI_PASSWORD;
static const char* host = IOT_CONFIG_IOTHUB_FQDN;
static const char* mqtt_broker_uri = "mqtts://" IOT_CONFIG_IOTHUB_FQDN;
static const char* device_id = IOT_CONFIG_DEVICE_ID;
static const int mqtt_port = AZ_IOT_DEFAULT_MQTT_CONNECT_PORT;

// Memory allocated for the sample's variables and structures.
static esp_mqtt_client_handle_t mqtt_client;
static az_iot_hub_client client;

static char mqtt_client_id[128];
static char mqtt_username[128];
static char mqtt_password[200];
static uint8_t sas_signature_buffer[256];
static unsigned long next_telemetry_send_time_ms = 0;
static char telemetry_topic[128];
static uint8_t telemetry_payload[100];


#define INCOMING_DATA_BUFFER_SIZE 128
static char incoming_data[INCOMING_DATA_BUFFER_SIZE];

// Auxiliary functions

static AzIoTSasToken sasToken(
    &client,
    AZ_SPAN_FROM_STR(IOT_CONFIG_DEVICE_KEY),
    AZ_SPAN_FROM_BUFFER(sas_signature_buffer),
    AZ_SPAN_FROM_BUFFER(mqtt_password));

static void connectToWiFi()
{
  Logger.Info("Estableciendo conexion WiFi " + String(ssid));

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");

  Logger.Info("WiFi conectado, IP: " + WiFi.localIP().toString());
}

static void initializeTime()
{
  Logger.Info("Setting time using SNTP");

  configTime(GMT_OFFSET_SECS, GMT_OFFSET_SECS_DST, NTP_SERVERS);
  time_t now = time(NULL);
  while (now < UNIX_TIME_NOV_13_2017)
  {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("");
  Logger.Info("Time initialized!");
}

void receivedCallback(char* topic, byte* payload, unsigned int length)
{
  Logger.Info("Received [");
  Logger.Info(topic);
  Logger.Info("]: ");
  for (int i = 0; i < length; i++)
  {
    Serial.print((char)payload[i]);
  }
  Serial.println("");
}

static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
  switch (event->event_id)
  {
    int i, r;

    case MQTT_EVENT_ERROR:
      Logger.Info("MQTT event MQTT_EVENT_ERROR");
      break;
    case MQTT_EVENT_CONNECTED:
      Logger.Info("MQTT event MQTT_EVENT_CONNECTED");

      r = esp_mqtt_client_subscribe(mqtt_client, AZ_IOT_HUB_CLIENT_C2D_SUBSCRIBE_TOPIC, 1);
      if (r == -1)
      {
        Logger.Error("Could not subscribe for cloud-to-device messages.");
      }
      else
      {
        Logger.Info("Subscribed for cloud-to-device messages; message id:"  + String(r));
      }

      break;
    case MQTT_EVENT_DISCONNECTED:
      Logger.Info("MQTT event MQTT_EVENT_DISCONNECTED");
      break;
    case MQTT_EVENT_SUBSCRIBED:
      Logger.Info("MQTT event MQTT_EVENT_SUBSCRIBED");
      break;
    case MQTT_EVENT_UNSUBSCRIBED:
      Logger.Info("MQTT event MQTT_EVENT_UNSUBSCRIBED");
      break;
    case MQTT_EVENT_PUBLISHED:
      Logger.Info("MQTT event MQTT_EVENT_PUBLISHED");
      break;
    case MQTT_EVENT_DATA:
      Logger.Info("MQTT event MQTT_EVENT_DATA");

      for (i = 0; i < (INCOMING_DATA_BUFFER_SIZE - 1) && i < event->topic_len; i++)
      {
        incoming_data[i] = event->topic[i]; 
      }
      incoming_data[i] = '\0';
      Logger.Info("Topic: " + String(incoming_data));
      
      for (i = 0; i < (INCOMING_DATA_BUFFER_SIZE - 1) && i < event->data_len; i++)
      {
        incoming_data[i] = event->data[i]; 
      }
      incoming_data[i] = '\0';
      Logger.Info("Data: " + String(incoming_data));

      break;
    case MQTT_EVENT_BEFORE_CONNECT:
      Logger.Info("MQTT event MQTT_EVENT_BEFORE_CONNECT");
      break;
    default:
      Logger.Error("MQTT event UNKNOWN");
      break;
  }

  return ESP_OK;
}

static void initializeIoTHubClient()
{
  az_iot_hub_client_options options = az_iot_hub_client_options_default();
  options.user_agent = AZ_SPAN_FROM_STR(AZURE_SDK_CLIENT_USER_AGENT);

  if (az_result_failed(az_iot_hub_client_init(
          &client,
          az_span_create((uint8_t*)host, strlen(host)),
          az_span_create((uint8_t*)device_id, strlen(device_id)),
          &options)))
  {
    Logger.Error("Failed initializing Azure IoT Hub client");
    return;
  }

  size_t client_id_length;
  if (az_result_failed(az_iot_hub_client_get_client_id(
          &client, mqtt_client_id, sizeof(mqtt_client_id) - 1, &client_id_length)))
  {
    Logger.Error("Failed getting client id");
    return;
  }

  if (az_result_failed(az_iot_hub_client_get_user_name(
          &client, mqtt_username, sizeofarray(mqtt_username), NULL)))
  {
    Logger.Error("Failed to get MQTT clientId, return code");
    return;
  }

  Logger.Info("Client ID: " + String(mqtt_client_id));
  Logger.Info("Username: " + String(mqtt_username));
}

static int initializeMqttClient()
{
  if (sasToken.Generate(SAS_TOKEN_DURATION_IN_MINUTES) != 0)
  {
    Logger.Error("Failed generating SAS token");
    return 1;
  }

  esp_mqtt_client_config_t mqtt_config;
  memset(&mqtt_config, 0, sizeof(mqtt_config));
  mqtt_config.uri = mqtt_broker_uri;
  mqtt_config.port = mqtt_port;
  mqtt_config.client_id = mqtt_client_id;
  mqtt_config.username = mqtt_username;
  mqtt_config.password = (const char*)az_span_ptr(sasToken.Get());
  mqtt_config.keepalive = 30;
  mqtt_config.disable_clean_session = 0;
  mqtt_config.disable_auto_reconnect = false;
  mqtt_config.event_handle = mqtt_event_handler;
  mqtt_config.user_context = NULL;
  mqtt_config.cert_pem = (const char*)ca_pem;
  mqtt_config.network_timeout_ms = 60000;
  mqtt_config.reconnect_timeout_ms = 30000;
  mqtt_config.skip_cert_common_name_check = true;

  mqtt_client = esp_mqtt_client_init(&mqtt_config);

  if (mqtt_client == NULL)
  {
    Logger.Error("Failed creating mqtt client");
    return 1;
  }

  esp_err_t start_result = esp_mqtt_client_start(mqtt_client);

  if (start_result != ESP_OK)
  {
    Logger.Error("Could not start mqtt client; error code:" + start_result);
    return 1;
  }
  else
  {
    Logger.Info("MQTT client started");
    return 0;
  }
}


static uint32_t getEpochTimeInSecs() 
{ 
  return (uint32_t)time(NULL);
}

static void establishConnection()
{
  connectToWiFi();
  initializeTime();
  initializeIoTHubClient();
  (void)initializeMqttClient();
}

// Función para obtener la carga útil de telemetría
static void getTelemetryPayload(az_span payload, az_span* out_payload)
{
  az_span original_payload = payload;


  // Obtener la fecha y hora actual
   // Obtener la fecha y hora actual
   //char current_time[64];
   //getCurrentTime(current_time, sizeof(current_time));

   // Construir el mensaje JSON con la fecha y hora
   //payload = az_span_copy(payload, AZ_SPAN_FROM_STR("{ \"Fecha\": \""));
   //payload = az_span_copy(payload, az_span_create_from_str(current_time));

  // Obtener tiempo actual en formato legible DD/MM/AAAA
  time_t now = time(NULL);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char timeStr[30];
  strftime(timeStr, sizeof(timeStr), "%d/%m/%Y", &timeinfo);

  // Agregar la fecha al payload
  payload = az_span_copy(payload, AZ_SPAN_FROM_STR("{ \"fecha\": \""));
  payload = az_span_copy(payload, az_span_create_from_str(timeStr));

  // Agregar los otros datos de telemetría
  payload = az_span_copy(payload, AZ_SPAN_FROM_STR("\", \"ruido\": "));
  (void)az_span_dtoa(payload, ruido, 2, &payload);

  payload = az_span_copy(payload, AZ_SPAN_FROM_STR(", \"ruido_corregido\": "));
  (void)az_span_dtoa(payload, ruidoCorregido2, 2, &payload);

  payload = az_span_copy(payload, AZ_SPAN_FROM_STR(", \"humedad\": "));
  (void)az_span_dtoa(payload, hum, 2, &payload);

  payload = az_span_copy(payload, AZ_SPAN_FROM_STR(", \"temperatura\": "));
  (void)az_span_dtoa(payload, tem, 2, &payload);

  payload = az_span_copy(payload, AZ_SPAN_FROM_STR(" }"));
  payload = az_span_copy_u8(payload, '\0');

  *out_payload = az_span_slice(original_payload, 0, az_span_size(original_payload) - az_span_size(payload));
}



static void sendTelemetry()
{
  az_span telemetry = AZ_SPAN_FROM_BUFFER(telemetry_payload);

  Logger.Info("Sending telemetry ...");

  // The topic could be obtained just once during setup,
  // however if properties are used the topic need to be generated again to reflect the
  // current values of the properties.
  if (az_result_failed(az_iot_hub_client_telemetry_get_publish_topic(
          &client, NULL, telemetry_topic, sizeof(telemetry_topic), NULL)))
  {
    Logger.Error("Failed az_iot_hub_client_telemetry_get_publish_topic");
    return;
  }

  getTelemetryPayload(telemetry, &telemetry);

  if (esp_mqtt_client_publish(
          mqtt_client,
          telemetry_topic,
          (const char*)az_span_ptr(telemetry),
          az_span_size(telemetry),
          MQTT_QOS1,
          DO_NOT_RETAIN_MSG)
      == 0)
  {
    Logger.Error("Failed publishing");
  }
  else
  {
    Logger.Info("Message published successfully");
  }
}


const int sampleWindow = 30;  // Ancho de ventana de muestra en ms (35 ms = 28.57Hz) 
unsigned int sample;


// Arduino setup and loop main functions.

void setup(){
  Serial.begin(115200);
  establishConnection();
  pinMode(sensorRuido, INPUT); // Microfono pin de entrada
  pinMode(Led, OUTPUT);        // Led pin de salida
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectToWiFi();
  } 
  else if (sasToken.IsExpired()) {
    Logger.Info("SAS token expired; reconnecting with a new one.");
    (void)esp_mqtt_client_destroy(mqtt_client);
    initializeMqttClient();
  } 
  else if (millis() > next_telemetry_send_time_ms) {
   
    // Lectura del sensor de sonido MAX4466
    unsigned long startMillis = millis();  // Inicio de la ventana de muestra
    float peakToPeak = 0;  // Amplitud pico-pico

    unsigned int signalMax = 0;   // Valor máximo de la señal
    unsigned int signalMin = 4095; // Valor mínimo de la señal para ESP32 (12-bit ADC)

    // Recolectar datos durante la ventana de muestra
    while (millis() - startMillis < sampleWindow) {
      sample = analogRead(sensorRuido);  // Obtener lectura del micrófono

      if (sample > signalMax) {
        signalMax = sample;  // Guardar el valor máximo
      }
      if (sample < signalMin) {
        signalMin = sample;  // Guardar el valor mínimo
      }
    }

    peakToPeak = signalMax - signalMin;  // Amplitud pico-pico
    int db = map(peakToPeak, 0, 4095, 40, 100);  // Ajustar según el sensor

    
    if (isnan(db)) {
      Serial.println("Error en la lectura del sensor MAX4466");
    }

    // Lectura del sensor DHT22 (Temperatura y Humedad)
    float temperatura = dht.readTemperature();
    float humedad = dht.readHumidity();

    tem = temperatura;
    hum = humedad;


    // Verificar si la lectura fue exitosa o si hubo un error
    if (isnan(temperatura) || isnan(humedad)) {
      Serial.println("Error de lectura del sensor DHT22");
      }

    else {
      // Cálculo de la corrección de ruido basada en la temperatura y la humedad
      float deltaDb = -0.01 * (temperatura - 20) + 0.02 * (humedad - 50);
      float dbCorregido = db + deltaDb;

      ruido = db;
      ruidoCorregido2 = dbCorregido;

      // 1. Imprimir Nivel de sonido
      Serial.print("Nivel de sonido: ");
      Serial.print(db);
      Serial.println(" dB");

      // 2. Imprimir Temperatura
      Serial.print("Temperatura: ");
      Serial.print(temperatura);
      Serial.println(" °C");

      // 3. Imprimir Humedad
      Serial.print("Humedad: ");
      Serial.print(humedad);
      Serial.println(" %");

      // 4. Imprimir Nivel de sonido corregido
      Serial.print("Nivel de sonido corregido: ");
      Serial.print(dbCorregido);
      Serial.println(" dB");

      // 5. Clasificación de rutas
      if (dbCorregido <= 60) {
        Serial.println("Ruta recomendable");
      } 
      else if (dbCorregido > 60 && dbCorregido <= 80) {
        Serial.println("Ruta riesgosa");
      } 
      else if (dbCorregido > 80) {
        Serial.println("Ruta no recomendable");

        // Si el nivel de decibeles es mayor a 80, hacer parpadear el LED
        Serial.println("Activando LED");
        digitalWrite(Led, HIGH);
        delay(500);
        digitalWrite(Led, LOW);
        delay(500);
      } 
      else {
        Serial.println("LED apagado");
        digitalWrite(Led, LOW);
      }
    }

    // Enviar telemetría
    sendTelemetry();

    // Actualizar el tiempo para el próximo envío de telemetría
    next_telemetry_send_time_ms = millis() + TELEMETRY_FREQUENCY_MILLISECS;
  } // Cierre del bloque else if
}  // Cierre de la función loop
