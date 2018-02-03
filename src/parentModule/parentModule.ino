//************************************************************
// 罠モジュール子機
// 子機、または親機とメッシュネットワークを形成する.
// また、子機自身がサーバーとしても起動しているため、
// 子機のネットワークに入り、子機のipアドレスにアクセスすると、
// 子機に対して各種設定をおこなうことができる
//************************************************************
#include <ESP8266WebServer.h>
#include <easyMesh.h>
#include <Milkcocoa.h>
#include <FS.h>
#include <WiFiClient.h>

/************************* 接続モジュール数確認用LED *************************/
#define LED 5
#define BLINK_PERIOD 1000000  // microseconds until cycle repeat
#define BLINK_DURATION 100000 // microseconds LED is on for
/************************* メッシュネットワーク設定 *************************/
#define MESH_PREFIX "trapModule"
#define MESH_PASSWORD "123456789"
#define MESH_PORT 5555

/************************* デフォルト設定値 *************************/
#define DEF_SLEEP_INTERVAL 600	// 10分間隔起動
#define DEF_WORK_TIME 180	// 3分間稼働
#define DEF_MODULE_NUM 1	// 接続台数
/************************* 設定値上限下限値 *************************/
#define MAX_SLEEP_INTERVAL 3600	// 60分
#define MIN_WORK_TIME 90	// 1分（これ以上短いと設定変更するためにアクセスする暇がない）
/************************* 設定値jsonName *************************/
#define JSON_SLEEP_INTERVAL "SleepInterval"
#define JSON_WORK_TIME "WorkTime"
#define JSON_MODULE_NUM "ModuleNum"
#define TRAP "Trap"
/************************* json buffer number *************************/
#define JSON_BUF_NUM 64

/************************* html *************************/
#define HTML_SLEEP_INTERVAL_NAME "sleepInterval"
#define HTML_WORK_TIME_NAME "workTime"
#define HTML_MODULE_NUM_NAME "moduleNum"

/************************* バッテリー関連 *************************/
// #define BATTERY_CHECK	// バッテリー残量チェックを行わない場合（分圧用抵抗が無いなど）はこの行をコメントアウト
#define DISCHARGE_END_VOLTAGE 600	// 放電終止電圧(1V)として1/6に分圧した場合の読み取り値
#define BATTERY_CHECK_INTERVAL 30000	// バッテリー残量チェック間隔(msec)

/************************* WiFi Access Point *********************************/
#define WLAN_SSID "YOUR_SSID"
#define WLAN_PASS "YOUR_SSID_PASS"

/************************* Your Milkcocoa Setup *********************************/
#define MILKCOCOA_APP_ID "milkcocoa_app_key"
#define MILKCOCOA_DATASTORE "datastore_name"

/************* Milkcocoa Setup (you don't need to change this!) ******************/
#define MILKCOCOA_SERVERPORT 1883

/************ Global State (you don't need to change this!) ******************/
// Create an ESP8266 WiFiClient class to connect to the MQTT server.
WiFiClient client;
const char MQTT_SERVER[] PROGMEM = MILKCOCOA_APP_ID ".mlkcca.com";
const char MQTT_CLIENTID[] PROGMEM = __TIME__ MILKCOCOA_APP_ID;
Milkcocoa milkcocoa = Milkcocoa(&client, MQTT_SERVER, MILKCOCOA_SERVERPORT, MILKCOCOA_APP_ID, MQTT_CLIENTID);

easyMesh mesh;
ESP8266WebServer server(80);

// 時間計測
unsigned long pastTime = 0;
unsigned long currentTime = 0;
// 放電終止電圧を下回ったらシャットダウン
boolean isBatteryEnough = true;

unsigned long messageReceiveTime = 0;

// モジュール設定
long _sleepInterval = 600;
long _workTime = 300;
int _moduleNum = 4;
bool _trap = false;

void setup()
{
	Serial.begin(115200);
	// 一度放電終止電圧を下回ったら確実に停止するために放電終止電圧に安全率をかける
	if (!checkBatteryRest(DISCHARGE_END_VOLTAGE * 1.05))
	{
		isBatteryEnough = false;
		setupWiFi();
		return;
	}
	// ファイルシステム
	SPIFFS.begin();
	pinMode(LED, OUTPUT);
	perseModuleSetting();

	//mesh.setDebugMsgTypes( ERROR | MESH_STATUS | CONNECTION | SYNC | COMMUNICATION | GENERAL | MSG_TYPES | REMOTE ); // all types on
	mesh.setDebugMsgTypes(ERROR | STARTUP); // set before init() so that you can see startup messages
	mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT);
	mesh.setReceiveCallback(&receivedCallback);
	mesh.setNewConnectionCallback(&newConnectionCallback);
	// http server 設定
	server.on("/", HTTP_GET, handleRoot);
	server.on("/", HTTP_POST, handlePost);
	server.begin();
	Serial.println("Server started");
}


void loop()
{
	if (!isBatteryEnough)
	{	// バッテリーが切れていたら終了
		milkcocoa.loop();
		DataElement dataElement = DataElement();
		int vcc = analogRead(A0);
		dataElement.setValue("Battery", String(vcc).c_str());
		milkcocoa.push(MILKCOCOA_DATASTORE, &dataElement);
		delay(10000);
		Serial.println("shut down...");
		ESP.deepSleep(0);
		return;
	}

	currentTime = millis();
	if ( currentTime - pastTime > BATTERY_CHECK_INTERVAL )
	{	// BATTERY_CHECK_INTERVALの間隔でバッテリー残量チェック
		// 放電終止電圧を下回ったら再起動して終了処理
		pastTime = currentTime;
		if ( !checkBatteryRest(DISCHARGE_END_VOLTAGE) )
		{
			Serial.println("Battery limit...");
			ESP.deepSleep(3 * 1000 * 1000);
		}
	}

	if ( _sleepInterval != 0 && millis() > _workTime * 1000)
	{
		Serial.println("move sleep mode...");
		ESP.deepSleep(_sleepInterval * 1000 * 1000);
	}

	mesh.update();
	server.handleClient();

	// run the blinky
	blinkLed(mesh.connectionCount());
}

/***************************************
	コールバック
****************************************/
// メッセージがあった場合のコールバック
void receivedCallback(uint32_t from, String &msg)
{
	messageReceiveTime = millis();
	perseMessage(msg);
}

// 新規メッシュネットワーク接続モジュールがあった場合のコールバック
void newConnectionCallback(bool adopt)
{
	Serial.printf("startHere: New Connection, adopt=%d\n", adopt);
}

// milkcocoa データ登録完了コールバック
void onpush(DataElement *elem)
{
	Serial.println("milkcocoa on push");
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
// 起動間隔：10分
// 稼働時間：3分
void setDefaultModuleSetting()
{
	StaticJsonBuffer<JSON_BUF_NUM> jsonBuf;
	JsonObject& defaultConfig = jsonBuf.createObject();
	defaultConfig[JSON_SLEEP_INTERVAL] = DEF_SLEEP_INTERVAL;
	defaultConfig[JSON_WORK_TIME] = DEF_WORK_TIME;
	defaultConfig[JSON_MODULE_NUM] = DEF_MODULE_NUM;
	saveModuleSetting(defaultConfig);
}

// モジュールに保存してある設定を読み出す
// ファイルが存在しない場合（初回起動時）はデフォルト値の設定ファイルを作成する
// 設定値はjson形式のファイルとする
void perseModuleSetting()
{
	File file = SPIFFS.open("/config.json", "r");
	if (!file)
	{
		setDefaultModuleSetting();
		return;
	}
	size_t size = file.size();
	if (size == 0)
	{
		file.close();
		setDefaultModuleSetting();
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
		return;
	}
	_sleepInterval = config[String(JSON_SLEEP_INTERVAL)];
	_sleepInterval = _sleepInterval > MAX_SLEEP_INTERVAL ? MAX_SLEEP_INTERVAL : _sleepInterval;
	_workTime = config[String(JSON_WORK_TIME)];
	_workTime = _workTime < MIN_WORK_TIME ? MIN_WORK_TIME : _workTime;
	_moduleNum = config[String(JSON_MODULE_NUM)];
	file.close();
	Serial.println("config read success");
}

// 設定ファイルを保存する
void saveModuleSetting(JsonObject &config)
{
	File file = SPIFFS.open("/config.json", "w");
	if (!file)
	{
		Serial.println("File Open Error");
		return;
	}
	config.printTo(file);
	file.close();
}

// モジュール間のメッセージを解析(json形式を想定)
void perseMessage(String &message)
{
	if (message == NULL || message.length())
		return;

	StaticJsonBuffer<JSON_BUF_NUM> jsonBuf;
	JsonObject &config = jsonBuf.parseObject(message);
	if (!config.success())
	{
		Serial.println("message perse fail");
		return;
	}
	saveModuleSetting(config);
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
	String sleepInterval = server.arg(HTML_SLEEP_INTERVAL_NAME);
	if (sleepInterval != NULL && sleepInterval.length() != 0)
	{
		config[JSON_SLEEP_INTERVAL] = sleepInterval.toInt();
		_sleepInterval = sleepInterval.toInt();
		_sleepInterval = _sleepInterval > MAX_SLEEP_INTERVAL ? MAX_SLEEP_INTERVAL : _sleepInterval;
	}
	String workTime = server.arg(HTML_WORK_TIME_NAME);
	if (workTime != NULL && workTime.length() != 0)
	{
		config[JSON_WORK_TIME] = workTime.toInt();
		_workTime = workTime.toInt();
		_workTime = (_workTime != 0 && _workTime < MIN_WORK_TIME) ? MIN_WORK_TIME : _workTime;
	}
	String moduleNum = server.arg(HTML_MODULE_NUM_NAME);
	if (moduleNum != NULL && moduleNum.length() != 0)
	{
		config[JSON_MODULE_NUM] = moduleNum.toInt();
		_moduleNum = moduleNum.toInt();
	}

	// テスト用
	// String trap = server.arg("trap");
	// if (trap != NULL && trap.length() != 0)
	// 	_trap = trap.toInt() == 0 ? false : true;

	String html = "";
	String message;
	config.printTo(message);
	if (mesh.sendBroadcast(message))
	{
		messageReceiveTime = millis();
		html += "<h1>Send Success</h1>";
	}
	else
	{
		html += "<h1>Send Fail</h1>";
	}
	html += createSettingHtml();

	saveModuleSetting(config);
	server.send(200, "text/html", html);
}

/************************************
	HW機能
************************************/
// 1/6に分圧した電圧が放電終止電圧1V * 4として
// vccLimit 放電終止電圧
// 放電終止電圧以上->true
// 放電終止電圧以下->false
boolean checkBatteryRest(int vccLimit)
{
	#ifndef BATTERY_CHECK
		return true;
	#endif

	#ifdef BATTERY_CHECK
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
	html += "    <h1>ModuleSetting</h1>";
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
	html += "            <span>ModuleNum</span>";
	html += "            <input type='number' name='moduleNum' placeholder='ModuleNum'>";
	html += "        </div>";
	html += "        <div>";
	html += "            <span>Trap</span>";
	html += "            <select name='trap'>";
	html += "            <option value='trapOff'>OFF</option>";
	html += "            <option value='trapOn'>ON</option>";
	html += "        </select>";
	html += "        </div>";
	html += "        <input type='submit'>";
	html += "    </form>";
	html += "</div>";
	html += "<div>";
	html += "    <h1>ModuleInfo</h1>";
	html += "    <span>ModuleID:";
	html += mesh.getChipId();
	html += "</span><br>";
	html += "    <span>SleepInterval(max:3600sec):";
	html += _sleepInterval;
	html += "</span><br>";
	html += "    <span>WokeTime(min:60sec):";
	html += _workTime;
	html += "</span><br>";
	html += "    <span>ModuleNum:";
	html += _moduleNum;
	html += "</span><br>";
	html += "    <span>ConnectionCount:";
	html += mesh.connectionCount();
	html += "</span><br>";
	html += "</div>";
	return html;
}