/*
 * Sistema de Controle de Lanche SENAI
 * Firmware ESP8266 + RFID RC522
 *
 * Dependências (Library Manager):
 *   - MFRC522 by GithubCommunity
 *   - ArduinoJson by Benoit Blanchon (v6+)
 *   - LittleFS (inclusa no core ESP8266)
 *   - ESP8266HTTPClient (inclusa no core ESP8266)
 *
 * Pinagem RC522 → ESP8266 (NodeMCU):
 *   SDA  → D2 (GPIO4)   SCK  → D5 (GPIO14)
 *   MOSI → D7 (GPIO13)  MISO → D6 (GPIO12)
 *   RST  → D1 (GPIO5)   3.3V → 3V3   GND → GND
 */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <time.h>

// ─── Configurações — edite aqui ──────────────────────────────────────────────
const char* WIFI_SSID     = "NOME_DA_REDE_IOT";   // SSID da rede WiFi IoT
const char* WIFI_PASSWORD = "SENHA_DA_REDE_IOT";
const char* SERVER_URL    = "https://SEU-PROJETO.up.railway.app/verificar";
const char* SYNC_URL      = "https://SEU-PROJETO.up.railway.app/sync-offline";
const char* CACHE_URL     = "https://SEU-PROJETO.up.railway.app/sync-cache";
const char* DEVICE_KEY    = "1ef31af6294ae254b58ddf7a24bc63a96eca46122f7fc1bfc4470e5f89cf6675";
// ─────────────────────────────────────────────────────────────────────────────

#define PIN_RST          5
#define PIN_SDA          4
#define PIN_LED_VERDE   16
#define PIN_LED_VERMELHO 15
#define PIN_BUZZER        0

#define DEBOUNCE_MS      2000
#define TIMEOUT_HTTP     6000
#define MAX_FILA_OFFLINE   50

MFRC522 rfid(PIN_SDA, PIN_RST);
unsigned long ultimaLeitura = 0;
bool wifiOk = false;

struct RegistroOffline {
  char uid[30];
  int  aluno_id;
  char aluno_nome[60];
  char aluno_turma[20];
  char status[10];
  char motivo[60];
  char criado_em[30];
};

RegistroOffline filaOffline[MAX_FILA_OFFLINE];
int filaCount = 0;

// ─── Protótipos ───────────────────────────────────────────────────────────────
void conectarWiFi();
void reconectarWiFi();
void verificarOnline(const String& uid);
void verificarOffline(const String& uid);
void sincronizarFila();
void baixarCacheOffline();
void enfileirarOffline(const String&, int, const char*, const char*, const char*, const char*);
void salvarFilaOffline();
void carregarFilaOffline();
void registrarHojeLocal(int);
String lerUID();
void ledVerde();
void ledVermelho();
void beepOk();
void beepErro();

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_LED_VERDE,    OUTPUT);
  pinMode(PIN_LED_VERMELHO, OUTPUT);
  pinMode(PIN_BUZZER,       OUTPUT);
  ledVermelho();

  SPI.begin();
  rfid.PCD_Init();
  delay(50);

  byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("[RFID] v0x%02X — %s\n", v, (v==0x00||v==0xFF)?"ERRO fiacao!":"OK");

  if (!LittleFS.begin()) {
    Serial.println("[FS] Erro — formatando...");
    LittleFS.format();
    LittleFS.begin();
  }

  carregarFilaOffline();

  Serial.println("\n=== SENAI Lanche RFID ===");
  conectarWiFi();

  if (wifiOk) {
    sincronizarFila();
    baixarCacheOffline();
  }

  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("[NTP] Sincronizando");
  time_t agora = 0;
  for (int i = 0; i < 20 && agora < 1000000; i++) { delay(500); Serial.print("."); agora = time(nullptr); }
  Serial.println(agora > 1000000 ? " OK" : " Falhou");
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiOk) { Serial.println("[WiFi] Conexao perdida."); wifiOk = false; ledVermelho(); }
    reconectarWiFi();
  } else if (!wifiOk) {
    wifiOk = true;
    Serial.println("[WiFi] Reconectado: " + WiFi.localIP().toString());
    sincronizarFila();
    baixarCacheOffline();
  }

  static unsigned long ultimoHB = 0;
  if (millis() - ultimoHB > 5000) {
    ultimoHB = millis();
    Serial.println("[loop] Aguardando cartao... WiFi:" + String(wifiOk?"OK":"OFF"));
  }

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  unsigned long agora = millis();
  if (agora - ultimaLeitura < DEBOUNCE_MS) { rfid.PICC_HaltA(); return; }
  ultimaLeitura = agora;

  String uid = lerUID();
  Serial.println("[RFID] UID: " + uid);

  if (wifiOk) verificarOnline(uid);
  else        verificarOffline(uid);

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

// ─── Verificação online ───────────────────────────────────────────────────────
void verificarOnline(const String& uid) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(TIMEOUT_HTTP);
  http.begin(client, SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_KEY);

  StaticJsonDocument<128> req;
  req["uid"] = uid; req["origem"] = "online";
  String body; serializeJson(req, body);

  int code = http.POST(body);
  Serial.println("[HTTP] " + String(code));

  if (code == HTTP_CODE_OK) {
    StaticJsonDocument<256> resp;
    if (!deserializeJson(resp, http.getString())) {
      if (resp["acesso"] | false) {
        Serial.println("[OK] " + String(resp["aluno"] | "Aluno"));
        beepOk(); ledVerde(); delay(2000); ledVermelho();
      } else {
        Serial.println("[NEG] " + String(resp["motivo"] | "Negado"));
        beepErro();
      }
    }
  } else if (code < 0) {
    Serial.println("[HTTP] Sem resposta — modo offline");
    verificarOffline(uid);
  } else {
    Serial.println("[HTTP] Erro " + String(code));
    beepErro();
  }
  http.end();
}

// ─── Verificação offline ──────────────────────────────────────────────────────
void verificarOffline(const String& uid) {
  Serial.println("[OFF] Verificando: " + uid);
  File f = LittleFS.open("/uid_cache.json", "r");
  if (!f) { Serial.println("[OFF] Sem cache"); beepErro(); enfileirarOffline(uid,0,"","","negado","Sem cache offline"); return; }

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) { Serial.println("[OFF] Cache corrompido"); beepErro(); return; }

  int aluno_id = 0; String nome = "", turma = ""; bool found = false;
  for (JsonObject u : doc["uids"].as<JsonArray>()) {
    if (String(u["uid"].as<const char*>()) == uid) {
      aluno_id = u["aluno_id"] | 0; nome = u["nome"] | String(""); turma = u["turma"] | String(""); found = true; break;
    }
  }

  if (!found) { Serial.println("[OFF] UID nao encontrado"); beepErro(); enfileirarOffline(uid,0,"","","negado","Cartao nao cadastrado"); return; }

  File fh = LittleFS.open("/hoje.json", "r");
  if (fh) {
    DynamicJsonDocument hoje(1024);
    if (!deserializeJson(hoje, fh)) {
      for (int id : hoje["ids"].as<JsonArray>()) {
        if (id == aluno_id) { fh.close(); Serial.println("[OFF] Ja retirou hoje"); beepErro(); enfileirarOffline(uid,aluno_id,nome.c_str(),turma.c_str(),"negado","Lanche ja retirado hoje"); return; }
      }
    }
    fh.close();
  }

  Serial.println("[OFF] Liberado: " + nome);
  beepOk(); ledVerde(); delay(2000); ledVermelho();
  registrarHojeLocal(aluno_id);
  enfileirarOffline(uid, aluno_id, nome.c_str(), turma.c_str(), "liberado", "");
}

// ─── Sync fila offline ────────────────────────────────────────────────────────
void sincronizarFila() {
  if (filaCount == 0) carregarFilaOffline();
  if (filaCount == 0) return;
  Serial.println("[SYNC] Enviando " + String(filaCount) + " registro(s)...");

  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.createNestedArray("registros");
  for (int i = 0; i < filaCount; i++) {
    JsonObject o = arr.createNestedObject();
    o["uid"]=filaOffline[i].uid; o["aluno_id"]=filaOffline[i].aluno_id;
    o["aluno_nome"]=filaOffline[i].aluno_nome; o["aluno_turma"]=filaOffline[i].aluno_turma;
    o["status"]=filaOffline[i].status; o["motivo"]=filaOffline[i].motivo; o["criado_em"]=filaOffline[i].criado_em;
  }
  String body; serializeJson(doc, body);

  WiFiClientSecure client; client.setInsecure(); HTTPClient http;
  http.setTimeout(8000);
  http.begin(client, SYNC_URL);
  http.addHeader("Content-Type","application/json");
  http.addHeader("X-Device-Key", DEVICE_KEY);
  int code = http.POST(body);
  if (code == HTTP_CODE_OK) {
    Serial.println("[SYNC] OK");
    filaCount = 0;
    LittleFS.remove("/fila_sync.json");
    LittleFS.remove("/hoje.json");
  } else {
    Serial.println("[SYNC] Falhou: " + String(code));
  }
  http.end();
}

// ─── Download cache de UIDs ───────────────────────────────────────────────────
void baixarCacheOffline() {
  Serial.print("[CACHE] Baixando UIDs...");
  WiFiClientSecure client; client.setInsecure(); HTTPClient http;
  http.setTimeout(8000);
  http.begin(client, CACHE_URL);
  http.addHeader("X-Device-Key", DEVICE_KEY);
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    String payload = http.getString();
    File f = LittleFS.open("/uid_cache.json", "w");
    if (f) { f.print(payload); f.close(); Serial.println(" OK (" + String(payload.length()) + "b)"); }
  } else {
    Serial.println(" Falhou: " + String(code));
  }
  http.end();
}

// ─── Fila offline helpers ─────────────────────────────────────────────────────
void enfileirarOffline(const String& uid, int id, const char* nome, const char* turma, const char* status, const char* motivo) {
  if (filaCount >= MAX_FILA_OFFLINE) { memmove(&filaOffline[0],&filaOffline[1],sizeof(RegistroOffline)*(MAX_FILA_OFFLINE-1)); filaCount--; }
  RegistroOffline& r = filaOffline[filaCount++];
  uid.toCharArray(r.uid, sizeof(r.uid));
  r.aluno_id = id;
  strncpy(r.aluno_nome, nome, sizeof(r.aluno_nome)-1);
  strncpy(r.aluno_turma, turma, sizeof(r.aluno_turma)-1);
  strncpy(r.status, status, sizeof(r.status)-1);
  strncpy(r.motivo, motivo, sizeof(r.motivo)-1);
  time_t agora = time(nullptr);
  struct tm* t = localtime(&agora);
  strftime(r.criado_em, sizeof(r.criado_em), "%Y-%m-%dT%H:%M:%S", t);
  salvarFilaOffline();
}

void salvarFilaOffline() {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.createNestedArray("registros");
  for (int i = 0; i < filaCount; i++) {
    JsonObject o = arr.createNestedObject();
    o["uid"]=filaOffline[i].uid; o["aluno_id"]=filaOffline[i].aluno_id;
    o["aluno_nome"]=filaOffline[i].aluno_nome; o["aluno_turma"]=filaOffline[i].aluno_turma;
    o["status"]=filaOffline[i].status; o["motivo"]=filaOffline[i].motivo; o["criado_em"]=filaOffline[i].criado_em;
  }
  File f = LittleFS.open("/fila_sync.json", "w");
  if (f) { serializeJson(doc, f); f.close(); }
}

void carregarFilaOffline() {
  File f = LittleFS.open("/fila_sync.json", "r");
  if (!f) return;
  DynamicJsonDocument doc(4096);
  if (!deserializeJson(doc, f)) {
    filaCount = 0;
    for (JsonObject o : doc["registros"].as<JsonArray>()) {
      if (filaCount >= MAX_FILA_OFFLINE) break;
      RegistroOffline& r = filaOffline[filaCount++];
      strncpy(r.uid,         o["uid"]         | "", sizeof(r.uid)-1);
      r.aluno_id = o["aluno_id"] | 0;
      strncpy(r.aluno_nome,  o["aluno_nome"]  | "", sizeof(r.aluno_nome)-1);
      strncpy(r.aluno_turma, o["aluno_turma"] | "", sizeof(r.aluno_turma)-1);
      strncpy(r.status,      o["status"]      | "", sizeof(r.status)-1);
      strncpy(r.motivo,      o["motivo"]      | "", sizeof(r.motivo)-1);
      strncpy(r.criado_em,   o["criado_em"]   | "", sizeof(r.criado_em)-1);
    }
    Serial.println("[FS] Fila: " + String(filaCount) + " item(s)");
  }
  f.close();
}

void registrarHojeLocal(int aluno_id) {
  DynamicJsonDocument doc(1024);
  File fr = LittleFS.open("/hoje.json", "r");
  if (fr) { deserializeJson(doc, fr); fr.close(); }
  if (!doc.containsKey("ids")) doc.createNestedArray("ids");
  doc["ids"].add(aluno_id);
  File fw = LittleFS.open("/hoje.json", "w");
  if (fw) { serializeJson(doc, fw); fw.close(); }
}

// ─── WiFi ─────────────────────────────────────────────────────────────────────
void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Conectando a " + String(WIFI_SSID));
  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t < 20) { delay(500); Serial.print("."); t++; }
  if (WiFi.status() == WL_CONNECTED) {
    wifiOk = true;
    Serial.println("\n[WiFi] OK — IP: " + WiFi.localIP().toString());
    Serial.println("[WiFi] Servidor: " + String(SERVER_URL));
    beepOk(); ledVerde(); delay(500); ledVermelho();
  } else {
    Serial.println("\n[WiFi] Falhou — modo offline");
  }
}

unsigned long proxTentativa = 0, intervalo = 5000;
void reconectarWiFi() {
  if (millis() < proxTentativa) return;
  WiFi.reconnect(); delay(2000);
  intervalo = (WiFi.status()==WL_CONNECTED) ? 5000 : min(intervalo*2,(unsigned long)60000);
  proxTentativa = millis() + intervalo;
}

// ─── RFID / LED / Buzzer ──────────────────────────────────────────────────────
String lerUID() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1) uid += ":";
  }
  uid.toUpperCase();
  return uid;
}

void ledVerde()    { digitalWrite(PIN_LED_VERDE, HIGH); digitalWrite(PIN_LED_VERMELHO, LOW); }
void ledVermelho() { digitalWrite(PIN_LED_VERDE, LOW);  digitalWrite(PIN_LED_VERMELHO, HIGH); }
void beepOk()      { tone(PIN_BUZZER, 1000, 150); delay(200); tone(PIN_BUZZER, 1500, 150); }
void beepErro()    { tone(PIN_BUZZER, 400, 400); }
