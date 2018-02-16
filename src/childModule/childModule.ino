//************************************************************
// 罠モジュール子機
// 子機、または親機とメッシュネットワークを形成する.
// また、子機自身がサーバーとしても起動しているため、
// 子機のネットワークに入り、子機のipアドレスにアクセスすると、
// 子機に対して各種設定をおこなうことができる
//************************************************************
#include <ESP8266WebServer.h>
#include <easyMesh.h>
#include <FS.h>
#include <WiFiClient.h>

/************************* 罠検知設定 *************************/
#define TRAP_CHECK
#define TRAP_IN 14
/************************* 罠設置モードでの強制起動用 *************************/
#define TRAP_SET_MODE_IN 12
/************************* 接続モジュール数確認用LED *************************/
#define LED 13
#define BLINK_PERIOD 1000000  // microseconds until cycle repeat
#define BLINK_DURATION 100000 // microseconds LED is on for
/************************* メッシュネットワーク設定 *************************/
#define MESH_PREFIX "trapModule"
#define MESH_PASSWORD "123456789"
#define MESH_PORT 5555
/************************* デフォルト設定値 *************************/
#define DEF_SLEEP_INTERVAL 3600	// 60分間隔起動
#define DEF_WORK_TIME 180	// 3分間稼働
#define DEF_TRAP_MODE false // 設置モード
#define DEF_TRAP_FIRE false // 罠作動済みフラグ
/************************* 設定値上限下限値 *************************/
#define MAX_SLEEP_INTERVAL 3600	// 60分
#define MIN_SLEEP_INTERVAL 10	// 1分
#define MIN_WORK_TIME 60	// 1分（これ以上短いと設定変更するためにアクセスする暇がない）
#define MAX_WORK_TIME 600	// 10分
/************************* 設定値jsonName *************************/
#define JSON_SLEEP_INTERVAL "SleepInterval"
#define JSON_WORK_TIME "WorkTime"
#define JSON_TRAP_MODE "TrapMode"	// o -> トラップ設置モード, 1 -> トラップ起動モード
#define JSON_TRAP_FIRE "trapFire"
// 設定値以外の Json
#define JSON_TRAP_FIRE_MESSAGE "trapFireMessage"
#define JSON_BATTERY_DEAD_MESSAGE "BatteryDeadMessage"
/************************* json buffer number *************************/
#define JSON_BUF_NUM 128
/************************* html *************************/
#define HTML_SLEEP_INTERVAL_NAME "sleepInterval"
#define HTML_WORK_TIME_NAME "workTime"
#define HTML_TRAP_MODE_NAME "trapMode"
/************************* バッテリー関連 *************************/
// #define BATTERY_CHECK	// バッテリー残量チェックを行わない場合（分圧用抵抗が無いなど）はこの行をコメントアウト
#define DISCHARGE_END_VOLTAGE 600	// 放電終止電圧(1V)として1/6に分圧した場合の読み取り値
#define BATTERY_CHECK_INTERVAL 30	// バッテリー残量チェック間隔(msec)
/************************* WiFi Access Point *********************************/
#define WLAN_SSID "YOUR_SSID"
#define WLAN_PASS "YOUR_SSID_PASS"

/************* モジュール設定初期値 ******************/
long _sleepInterval = DEF_SLEEP_INTERVAL;
long _workTime = DEF_WORK_TIME;
bool _trapMode = DEF_TRAP_MODE;
bool _trapFire = DEF_TRAP_FIRE;

easyMesh mesh;
uint32 parentChipId = 0;
ESP8266WebServer server(80);

// 時間計測
unsigned long pastTime = 0;
// 放電終止電圧を下回ったらシャットダウン
boolean isBatteryEnough = true;
bool isTrapStart = false;

void setup()
{
	Serial.begin(115200);
	// ファイルシステム
	SPIFFS.begin();

	#ifdef TRAP_CHECK
		pinMode(TRAP_IN, INPUT);
	#endif
	// 一度放電終止電圧を下回ったら確実に停止するために放電終止電圧に安全率をかける
	// if (!checkBattery(DISCHARGE_END_VOLTAGE * 1.05, false))
	// {
	// 	isBatteryEnough = false;
	// 	setupWiFi();
	// 	return;
	// }
	
	pinMode(LED, OUTPUT);
	pinMode(TRAP_SET_MODE_IN, INPUT);
	readModuleSettingFile();
	// 罠設置モードスイッチが押下された状態で起動した場合設定値を上書き
	if (digitalRead(TRAP_SET_MODE_IN)) {
		_trapMode = false;
		saveCurrentModuleSeting();
	}

	//mesh.setDebugMsgTypes( ERROR | MESH_STATUS | CONNECTION | SYNC | COMMUNICATION | GENERAL | MSG_TYPES | REMOTE ); // all types on
	mesh.setDebugMsgTypes(ERROR | STARTUP); // set before init() so that you can see startup messages
	mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT);
	mesh.setReceiveCallback(&receivedCallback);
	mesh.setNewConnectionCallback(&newConnectionCallback);
	// http server 設定
	server.on("/", HTTP_GET, handleRoot);
	server.on("/", HTTP_POST, handlePost);
	server.begin();
	Serial.println("Trap Module Start");
}


void loop()
{
	// if (!isBatteryEnough)
	// {	// バッテリーが切れていたら終了
	// 	milkcocoa.loop();
	// 	DataElement dataElement = DataElement();
	// 	int vcc = analogRead(A0);
	// 	dataElement.setValue("Battery", String(vcc).c_str());
	// 	milkcocoa.push(MILKCOCOA_DATASTORE, &dataElement);
	// 	delay(10000);
	// 	Serial.println("shut down...");
	// 	ESP.deepSleep(0);
	// 	return;
	// }

	if ( !checkBattery(DISCHARGE_END_VOLTAGE, true) )
	{
		Serial.println("Battery limit...");
		ESP.deepSleep(0);
		// ESP.deepSleep(3 * 1000 * 1000);
	}
	if (isTrapStart) {
		beginTrapModeLed();
		ESP.deepSleep(_sleepInterval * 1000 * 1000);
	}

	if ( _trapMode && _sleepInterval != 0 && millis() > _workTime * 1000)
	{
		Serial.println("move sleep mode...");
		ESP.deepSleep(_sleepInterval * 1000 * 1000);
	}

	#ifdef TRAP_CHECK
		// TODO: メッシュネットワーク数が規定数になったら、もしくは親モジュールと接続したら検知情報を送信
		// 起動 30 秒後から検知情報を送信(起動直後はメッシュネットワークが不安定かもしれない)
		if ( !_trapFire && _trapMode && millis() > 30 * 1000 && digitalRead(TRAP_IN)) {
			Serial.println("trap fire");
			_trapFire = true;
			sendTrapFire();
			saveCurrentModuleSeting();
		}
	#endif

	mesh.update();
	server.handleClient();

	// run the blinky
	if (!_trapMode) {
		blinkLed(mesh.connectionCount());
	} else {
		digitalWrite(LED, HIGH);
	}
}

/***************************************
	コールバック
****************************************/
// メッセージがあった場合のコールバック
void receivedCallback(uint32_t from, String &msg)
{
	// メッセージを Json 化
	StaticJsonBuffer<JSON_BUF_NUM> jsonBuf;
	JsonObject &jsonMessage = jsonBuf.parseObject(msg);
	if (!jsonMessage.success()) {
		return;
	}
	// 子モジュールはバッテリー切れ端末が発生しても特に何もしない
	if (jsonMessage.containsKey(JSON_BATTERY_DEAD_MESSAGE)) {
		Serial.println("battery dead message");
		return;
	}
	// 罠検知メッセージ受信（子モジュールは何もしない）
	if (jsonMessage.containsKey(JSON_TRAP_FIRE_MESSAGE)) {
		Serial.println("trap fire message");
		return;
	}
	// 罠検知済みフラグは自身の設定値を反映
	jsonMessage[JSON_TRAP_FIRE] = _trapFire;
	boolean preTrapMode = _trapMode;
	updateModuleSetting(jsonMessage);
	saveCurrentModuleSeting();
	// 設置モードから罠モードへ移行
	if (!preTrapMode && _trapMode) {
		isTrapStart = true;
	}
}

// 新規メッシュネットワーク接続モジュールがあった場合のコールバック
void newConnectionCallback(bool adopt)
{
	Serial.printf("startHere: New Connection, adopt=%d\n", adopt);
}

// wifi セットアップ
void setupWiFi()
{
	WiFi.mode(WIFI_AP_STA);
	Serial.print("Connecting to ");
	Serial.println(WLAN_SSID);

	WiFi.begin(WLAN_SSID, WLAN_PASS);
	while (WiFi.status() != WL_CONNECTED)
	{
		delay(500);
		Serial.print(".");
	}
	Serial.println("\nWiFi connected");
	Serial.println("IP address: ");
	Serial.println(WiFi.localIP());
}

/****************************************
	端末設定入出力
*****************************************/
// デフォルト設定を書き込む
void setDefaultModuleSetting()
{
	StaticJsonBuffer<JSON_BUF_NUM> jsonBuf;
	JsonObject& defaultConfig = jsonBuf.createObject();
	defaultConfig[JSON_SLEEP_INTERVAL] = DEF_SLEEP_INTERVAL;
	defaultConfig[JSON_WORK_TIME] = DEF_WORK_TIME;
	defaultConfig[JSON_TRAP_FIRE] = DEF_TRAP_FIRE;
}

// モジュールに保存してある設定を読み出す
// ファイルが存在しない場合（初回起動時）はデフォルト値の設定ファイルを作成する
// 設定値はjson形式のファイルとする
void readModuleSettingFile()
{
	File file = SPIFFS.open("/config.json", "r");
	if (!file)
	{
		setDefaultModuleSetting();
		saveCurrentModuleSeting();
		return;
	}
	size_t size = file.size();
	if (size == 0)
	{
		file.close();
		setDefaultModuleSetting();
		saveCurrentModuleSeting();
		return;
	}

	std::unique_ptr<char[]> buf(new char[size]);
	file.readBytes(buf.get(), size);
	StaticJsonBuffer<JSON_BUF_NUM> jsonBuffer;
	JsonObject &config = jsonBuffer.parseObject(buf.get());
	if (!config.success())
	{
		Serial.println("config read fail");
		file.close();
		setDefaultModuleSetting();
		saveCurrentModuleSeting();
		return;
	}
	updateModuleSetting(config);
	file.close();
	saveCurrentModuleSeting();
	Serial.println("config read success");
}

/**
 * モジュール設定値を更新(設定可能範囲を考慮)
 */
void updateModuleSetting(JsonObject& config) {
	if (!config.success()) {
		setDefaultModuleSetting();
	}
	// 起動間隔
	if (config[JSON_SLEEP_INTERVAL] < MIN_SLEEP_INTERVAL) {
		_sleepInterval = MIN_SLEEP_INTERVAL;
	} else if (config[JSON_SLEEP_INTERVAL] > MAX_SLEEP_INTERVAL) {
		_sleepInterval = MAX_SLEEP_INTERVAL;
	} else {
		_sleepInterval = config[JSON_SLEEP_INTERVAL];
	}
	// 稼働時間
	if (config[JSON_WORK_TIME] < MIN_WORK_TIME) {
		_workTime = MIN_WORK_TIME;
	} else if (config[JSON_WORK_TIME] > MAX_WORK_TIME) {
		_workTime = MAX_WORK_TIME;
	} else {
		_workTime = config[JSON_WORK_TIME];
	}
	// 罠モード
	_trapMode = config[JSON_TRAP_MODE];
	// 罠検知済みフラグ
	_trapFire = _trapMode ? config[JSON_TRAP_FIRE] : false;
}

// 設定ファイルを保存する
boolean saveModuleSetting(const JsonObject &config)
{
	File file = SPIFFS.open("/config.json", "w");
	if (!file)
	{
		Serial.println("File Open Error");
		return false;
	}
	config.printTo(file);
	file.close();
	return true;
}

/**
 * 現在の設定値を保存
 **/
boolean saveCurrentModuleSeting() {
	StaticJsonBuffer<JSON_BUF_NUM> jsonBuf;
	JsonObject &config = jsonBuf.createObject();
	config[JSON_TRAP_MODE] = _trapMode;
	config[JSON_SLEEP_INTERVAL] = _sleepInterval;
	config[JSON_WORK_TIME] = _workTime;
	config[JSON_TRAP_FIRE] = _trapFire;
	return saveModuleSetting(config);
}

// 端末電池切れ通知
void sendBatteryDead() {
	StaticJsonBuffer<JSON_BUF_NUM> jsonBuf;
	JsonObject &batteryDead = jsonBuf.createObject();
	batteryDead[JSON_BATTERY_DEAD_MESSAGE] = mesh.getChipId();
	String message;
	batteryDead.printTo(message);
	mesh.sendBroadcast(message);
}

/**
 * トラップ通知送信
 **/
void sendTrapFire() {
	StaticJsonBuffer<JSON_BUF_NUM> jsonBuf;
	JsonObject &trapFire = jsonBuf.createObject();
	trapFire[JSON_TRAP_FIRE_MESSAGE] = mesh.getChipId();
	String message;
	trapFire.printTo(message);
	mesh.sendBroadcast(message);
}

void beginTrapModeLed() {
	unsigned long blink = millis();
	uint8_t temp = 1;
	digitalWrite(LED, HIGH);
	delay(5000);
	digitalWrite(LED, LOW);
}

/************************************
	サーバー設定
************************************/
// 設定変更画面イベント
void handleRoot()
{
	String html = createSettingHtml();
	server.send(200, "text/html", html);
}

// 設定を変更された
void handlePost()
{
	StaticJsonBuffer<JSON_BUF_NUM> jsonBuf;
	JsonObject &config = jsonBuf.createObject();

	// 設定値反映
	// 起動間隔
	String sleepInterval = server.arg(HTML_SLEEP_INTERVAL_NAME);
	if (sleepInterval != NULL && sleepInterval.length() != 0)
	{
		config[JSON_SLEEP_INTERVAL] = sleepInterval.toInt();
	} else {
		config[JSON_SLEEP_INTERVAL] = _sleepInterval;
	}
	// 稼働時間
	String workTime = server.arg(HTML_WORK_TIME_NAME);
	if (workTime != NULL && workTime.length() != 0)
	{
		config[JSON_WORK_TIME] = workTime.toInt();
	} else {
		config[JSON_WORK_TIME] = _workTime;
	}
	// 罠モード
	String trapMode = server.arg(HTML_TRAP_MODE_NAME);
	if (trapMode != NULL && trapMode.length() != 0)
	{
		config[JSON_TRAP_MODE] = trapMode.toInt();
	} else {
		config[JSON_TRAP_MODE] = _trapMode;
	}

	// 設定値を全モジュールに反映
	String html = "";
	boolean preTrapMode = _trapMode;
	if (updateAllModuleSettings(config)) {
		updateModuleSetting(config);
		saveCurrentModuleSeting();
		html += "<h1>Set Success</h1>";
		if (!preTrapMode && _trapMode) {
			html += "<h1>Module Sleep After 5 sec...</h1>";
		}
	} else {
		html += "<h1>Set Fail</h1>";
	}
	html += createSettingHtml();
	server.send(200, "text/html", html);
	// 設置モードから罠モードへ移行
	if (!preTrapMode && _trapMode) {
		isTrapStart = true;
	}
}

/**
 * 設定値を全モジュールに反映する
 **/
boolean updateAllModuleSettings(const JsonObject& config) {
	String message;
	config.printTo(message);
	uint8_t retryCount = 5;
	while (retryCount > 0) {
		if (mesh.sendBroadcast(message)) {
			return true;
		}
		--retryCount;
		delay(5000);
	}
	return false;
}

/************************************
	HW機能
************************************/
/**
 * 1/6に分圧した電圧が放電終止電圧1V * 4としてバッテリーが
 * 放電終止電圧を下回っていないか[BATTERY_CHECK_INTERVAL]秒毎にチェックする
 * vccLimit 放電終止電圧
 * isInterval true:一定時間毎にチェック false:直ちにチェック
 * 放電終止電圧以上->true
 * 放電終止電圧以下->false
 **/
boolean checkBattery(int vccLimit, boolean isInterval)
{
	#ifndef BATTERY_CHECK
		return true;
	#endif

	#ifdef BATTERY_CHECK
		if (isInterval && millis() - pastTime < BATTERY_CHECK_INTERVAL * 1000 ) {
			return true;
		}
		pastTime = millis();
		int vcc = analogRead(A0);
		if (vcc > vccLimit)
			return true;

		return false;
	#endif
}

// メッシュネットワークを形成しているモジュール数だけ
// LEDを点滅させる
void blinkLed(uint8_t meshNodeCount)
{
	// run the blinky
	bool onFlag = false;
	uint32_t cycleTime = mesh.getNodeTime() % BLINK_PERIOD;
	for (uint8_t i = 0; i < (meshNodeCount + 1); i++)
	{
		uint32_t onTime = BLINK_DURATION * i * 2;

		if (cycleTime > onTime && cycleTime < onTime + BLINK_DURATION)
			onFlag = true;
	}
	digitalWrite(LED, onFlag);
}

// html生成
String createSettingHtml()
{
	String html = "<div>";
	html += "    <h1>Child Module</h1>";
	html += "    <form method='post'>";
	html += "        <div>";
	html += "            <span>SleepInterval(s)</span>";
	html += "            <input type='number' name='sleepInterval' placeholder='SleepInterval'>";
	html += "        </div>";
	html += "        <div>";
	html += "            <span>WorkTime(s)</span>";
	html += "            <input type='number' name='workTime' placeholder='WorkTime'>";
	html += "        </div>";
	html += "        <div>";
	html += "            <span>Trap</span>";
	html += "            <select name='trapMode'>";
	if (_trapMode) {
		html += "                <option value='0'>Trap Set Mode</option>";
		html += "                <option value='1' selected>Trap Start</option>";
	} else {
		html += "                <option value='0' selected>Trap Set Mode</option>";
		html += "                <option value='1'>Trap Start</option>";
	}
	html += "            </select>";
	html += "        </div>";
	html += "        <input type='submit'>";
	html += "    </form>";
	html += "</div>";
	html += "<div>";
	html += "    <h1>ModuleInfo</h1>";
	html += "    <span>TrapMode:";
	html += _trapMode ? "Trap Start Mode" : "Tarp Set Mode" ;
	html += "    </span><br>";
	html += "    <span>ModuleID:";
	html += mesh.getChipId();
	html += "    </span><br>";
	html += "    <span>TrapFire:";
	html += _trapFire ? "Fired" : "Not Fired";
	html += "    </span><br>";
	html += "    <span>SleepInterval(max:3600sec):";
	html += _sleepInterval;
	html += "    </span><br>";
	html += "    <span>WokeTime(min:60sec):";
	html += _workTime;
	html += "    </span><br>";
	html += "    <span>ConnectionCount:";
	html += mesh.connectionCount() + 1;
	html += "    </span>";
	html += "    <ul>";
	html += "        <li>";
	html += mesh.getChipId();
	html += "        </li>";
	SimpleList<meshConnectionType>::iterator connection = mesh._connections.begin();
    while ( connection != mesh._connections.end() ) {
		html += "<li>";
        html += connection->chipId;
		html += "</li>";
        connection++;
    }
	html += "    </ul>";
	html += "</div>";
	return html;
}