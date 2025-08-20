/*
Wireless Expression Pedal HAT for M5StickC

Board Manager: esp32 by Espressif Systems v2.0.17 - M5Stick-C
Libraries:
  M5Unified v0.2.5
  NimBLE-Arduino v1.4.3
  BLE-MIDI by lathoub v2.2
  MIDI Library by Francois Best, lathoub v5.0.2
*/

#include <M5Unified.h>
#include <Preferences.h>  // ESP32の不揮発性メモリ

// NimBLEを使用するための定義（BLEMIDI_Transport.hより前に記述）
#define BLE_MIDI_TRANSPORT_NIMBLE

#include <BLEMIDI_Transport.h>
#include <hardware/BLEMIDI_ESP32_NimBLE.h>

// BLE-MIDIインスタンス作成
BLEMIDI_CREATE_INSTANCE("M5EXP-MIDI", MIDI)

// ピン設定
const int ANALOG_PIN = 36;  // G36ピン or G26ピン  (HAT側)

// MIDI設定
const uint8_t MIDI_CHANNEL = 1;
const uint8_t CC_NUMBER = 1;    // モジュレーションホイール

// 液晶設定
const int LCD_BRIGHTNESS = 64;  // 0-255の範囲（デフォルト128の半分で節電）

// アナログ値のフィルタリング用
int lastValue = 0;
const int THRESHOLD = 5;  // ノイズ除去のための閾値

// BLE接続状態管理
bool isConnected = false;
bool displayNeedsUpdate = true;  // 画面更新フラグ
unsigned long lastConnectionCheck = 0;
const unsigned long CONNECTION_CHECK_INTERVAL = 1000; // 1秒ごとにチェック

// バッテリー監視用
unsigned long lastBatteryCheck = 0;
const unsigned long BATTERY_CHECK_INTERVAL = 60000; // 1分ごとにチェック
int batteryPercent = 100;
bool batteryInitialized = false; // 起動時の初期化フラグ

// 非同期バッテリー測定用
float batteryVoltageSum = 0;
int batteryMeasurementCount = 0;
const int BATTERY_SAMPLES = 3;  // 測定回数を3回に削減
unsigned long lastBatteryMeasurement = 0;
const unsigned long BATTERY_MEASUREMENT_INTERVAL = 100; // 100msごとに1回測定
float currentBatteryVoltage = 0;  // 現在の電圧値（表示用）
int m5BatteryLevel = 0;  // M5.Power.getBatteryLevel()の値（表示用）

// 画面レイアウト定数
const int SCREEN_WIDTH = 160;
const int SCREEN_HEIGHT = 80;
const int LINE_HEIGHT = 12;

// 関数プロトタイプ宣言
void drawBattery();
void drawConnectionStatus();
void updateMainDisplay();
int getBatteryPercent();

// ペダル校正クラス
class PedalCalibrator {
private:
    Preferences prefs;
    int minVal = 1750;  // デフォルト値
    int maxVal = 3840;
    bool calibrationMode = false;
    unsigned long calibrationStart = 0;
    const unsigned long CALIBRATION_TIME = 5000; // 5秒間
    
public:
    void begin() {
        prefs.begin("pedal", false);
        // 保存済みの値を読み込み
        minVal = prefs.getInt("minVal", 1750);
        maxVal = prefs.getInt("maxVal", 3840);
        Serial.printf("Loaded calibration: min=%d, max=%d\n", minVal, maxVal);
    }
    
    void startCalibration() {
        calibrationMode = true;
        calibrationStart = millis();
        // 現在の値で初期化（最初の読み取り値から開始）
        int currentValue = analogRead(ANALOG_PIN);
        minVal = currentValue;
        maxVal = currentValue;
        Serial.println("Calibration started - move pedal fully for 5 seconds!");
        Serial.printf("Starting with current value: %d\n", currentValue);
        
        // 画面表示
        M5.Display.clear();
        drawBattery(); // バッテリー表示は維持
        M5.Display.drawString("Calibration Mode", 5, 10);
        M5.Display.drawString("Move pedal fully!", 5, 25);
    }
    
    bool updateCalibration(int rawValue) {
        if (!calibrationMode) return false;
        
        // 範囲を更新
        minVal = min(minVal, rawValue);
        maxVal = max(maxVal, rawValue);
        
        // 進捗表示
        unsigned long elapsed = millis() - calibrationStart;
        int progress = (elapsed * 100) / CALIBRATION_TIME;
        
        // 進捗エリアをクリア（バッテリー表示は保持）
        M5.Display.fillRect(0, 40, SCREEN_WIDTH-30, 40, BLACK);
        M5.Display.drawString("Progress: " + String(progress) + "%", 5, 40);
        M5.Display.drawString("Min:" + String(minVal), 5, 52);
        M5.Display.drawString("Max:" + String(maxVal), 5, 64);
        
        // 校正完了チェック
        if (elapsed >= CALIBRATION_TIME) {
            finishCalibration();
            return true;
        }
        return false;
    }
    
    void finishCalibration() {
        calibrationMode = false;
        
        // 値を保存
        prefs.putInt("minVal", minVal);
        prefs.putInt("maxVal", maxVal);
        
        Serial.printf("Calibration completed: min=%d, max=%d\n", minVal, maxVal);
        
        // 画面表示
        M5.Display.clear();
        drawBattery();
        M5.Display.drawString("Calibration Done!", 5, 10);
        M5.Display.drawString("Min: " + String(minVal), 5, 25);
        M5.Display.drawString("Max: " + String(maxVal), 5, 40);
        delay(2000);  // 2秒間表示
        
        // 通常画面に戻す
        updateMainDisplay();
        displayNeedsUpdate = true;  // 校正後は画面更新フラグをリセット
    }
    
    int getMidiValue(int rawValue) {
        int constrainedValue = constrain(rawValue, minVal, maxVal);
        return map(constrainedValue, minVal, maxVal, 0, 127);
    }
    
    bool isCalibrating() { return calibrationMode; }
    
    int getMinVal() { return minVal; }
    int getMaxVal() { return maxVal; }
};

// グローバルインスタンス
PedalCalibrator calibrator;

// バッテリー残量を計算（電圧値から）
int calculateBatteryPercent(float voltage) {
    // より実用的な電圧範囲: 3.3V(0%) - 4.1V(100%) - mV単位
    const float MIN_VOLTAGE = 3300;  // mV単位 - シャットダウン間近
    const float MAX_VOLTAGE = 4100;  // mV単位 - 実用的な満充電レベル
    
    if (voltage < MIN_VOLTAGE) return 0;
    if (voltage > MAX_VOLTAGE) return 100;
    
    // 非線形変換（リチウム電池の放電特性に近似）
    float normalizedVoltage = (voltage - MIN_VOLTAGE) / (MAX_VOLTAGE - MIN_VOLTAGE);
    
    // 簡易的な非線形カーブ（3.7V付近での急速な低下を考慮）
    int percent;
    if (normalizedVoltage > 0.6) {
        // 高電圧域（3.78V以上）：比較的リニア
        percent = (int)(60 + (normalizedVoltage - 0.6) * 100);
    } else {
        // 低電圧域（3.78V以下）：急速に低下
        percent = (int)(normalizedVoltage * normalizedVoltage * 150);
    }
    
    return constrain(percent, 0, 100);
}

// 非同期バッテリー測定（MIDI処理を妨げない）
void updateBatteryAsync() {
    unsigned long currentTime = millis();
    
    // 測定タイミングチェック
    if (currentTime - lastBatteryMeasurement >= BATTERY_MEASUREMENT_INTERVAL) {
        lastBatteryMeasurement = currentTime;
        
        // 1回の測定を実行（delay()なし）
        float voltage = M5.Power.getBatteryVoltage();
        batteryVoltageSum += voltage;
        batteryMeasurementCount++;
        
        // 必要な測定回数に達したら平均を計算
        if (batteryMeasurementCount >= BATTERY_SAMPLES) {
            float averageVoltage = batteryVoltageSum / batteryMeasurementCount;
            currentBatteryVoltage = averageVoltage;  // 表示用に保存
            batteryPercent = calculateBatteryPercent(averageVoltage);
            
            // M5.Power.getBatteryLevel()の値も取得
            m5BatteryLevel = M5.Power.getBatteryLevel();
            
            // デバッグ情報出力
            static unsigned long lastDebugPrint = 0;
            if (currentTime - lastDebugPrint > 30000) { // 30秒ごと
                lastDebugPrint = currentTime;
                Serial.printf("Battery: %.2fV -> %d%% (M5Level: %d)\n", averageVoltage, batteryPercent, m5BatteryLevel);
            }
            
            // 測定値をリセット
            batteryVoltageSum = 0;
            batteryMeasurementCount = 0;
            lastBatteryCheck = currentTime;
        }
    }
}

// バッテリー表示を描画
void drawBattery() {
    // 右上にバッテリー残量を表示
    int batteryX = SCREEN_WIDTH - 25;
    int batteryY = 2;
    
    // 古いバッテリー表示をクリア（拡張エリア）
    //M5.Display.fillRect(batteryX - 20, batteryY, 45, 36, BLACK);
    M5.Display.fillRect(batteryX - 28, batteryY, 53, 36, BLACK);
    
    // バッテリー残量に応じて色を変更
    uint16_t batteryColor = GREEN;
    if (batteryPercent < 20) batteryColor = RED;        // 20%未満で赤
    else if (batteryPercent < 40) batteryColor = YELLOW; // 40%未満で黄色
    
    // 低電圧警告（15%以下で点滅）
    static bool warningBlink = false;
    static unsigned long lastBlinkTime = 0;
    if (batteryPercent < 15 && millis() - lastBlinkTime > 500) {
        warningBlink = !warningBlink;
        lastBlinkTime = millis();
        if (warningBlink) batteryColor = RED;
        else batteryColor = BLACK; // 点滅効果
    }
    
    // バッテリーアイコンを描画
    M5.Display.drawRect(batteryX, batteryY, 20, 8, WHITE);
    M5.Display.fillRect(batteryX + 20, batteryY + 2, 2, 4, WHITE);
    
    // バッテリー残量を塗りつぶし
    int fillWidth = (batteryPercent * 18) / 100;
    if (fillWidth > 0) {
        M5.Display.fillRect(batteryX + 1, batteryY + 1, fillWidth, 6, batteryColor);
    }
    
    // パーセンテージを表示
    M5.Display.setTextSize(1);
    String percentText = String(batteryPercent) + "%";
    M5.Display.drawString(percentText, batteryX - 26, batteryY);
    
    // 実際の電圧値を表示（デバッグ用）- mV単位を分かりやすく表示
    // String voltageText = String(currentBatteryVoltage / 1000.0, 2) + "V";
    // M5.Display.drawString(voltageText, batteryX - 18, batteryY + 12);
    
    // M5.Power.getBatteryLevel()の値を表示（デバッグ用）
    // String m5LevelText = "M5:" + String(m5BatteryLevel);
    // M5.Display.drawString(m5LevelText, batteryX - 18, batteryY + 24);
}

// メイン画面の表示を更新
void updateMainDisplay() {
    M5.Display.clear();
    drawBattery();
    
    // タイトル
    M5.Display.drawString("M5EXP-MIDI", 5, 2);
    
    // 接続状態（背景色付き）
    drawConnectionStatus();
    
    // 操作説明
    M5.Display.drawString("BtnB: Calibrate", 5, 65);
}

// 接続状態を背景色付きで表示
void drawConnectionStatus() {
    int statusX = 5;
    int statusY = 15;
    int statusWidth = 70;  // 背景の幅
    int statusHeight = 12; // 背景の高さ
    
    if (isConnected) {
        // Connected時：暗めの水色背景
        uint16_t darkCyan = M5.Display.color565(0, 90, 220);  // 水色
        M5.Display.fillRect(statusX - 2, statusY - 1, statusWidth, statusHeight, darkCyan);
        M5.Display.setTextColor(WHITE);
        M5.Display.drawString("Connected", statusX, statusY);
    } else {
        // Waiting時：黒背景
        M5.Display.fillRect(statusX - 2, statusY - 1, statusWidth, statusHeight, BLACK);
        M5.Display.setTextColor(WHITE);
        M5.Display.drawString("Waiting...", statusX, statusY);
    }
    
    // テキスト色を白に戻す（他の表示への影響を防ぐ）
    M5.Display.setTextColor(WHITE);
}

void setup() {
  // M5StickC初期化
  auto cfg = M5.config();
  M5.begin(cfg);
  
  M5.Display.setRotation(1);
  M5.Display.setTextSize(1);
  
  // 液晶明度設定（節電のため）
  M5.Display.setBrightness(LCD_BRIGHTNESS);
  Serial.printf("LCD Brightness set to: %d (0-255)\n", LCD_BRIGHTNESS);
  
  // 初期画面表示
  updateMainDisplay();
  M5.Display.drawString("Initializing...", 5, 28);
  
  Serial.begin(115200);
  
  // アナログピン設定
  pinMode(ANALOG_PIN, INPUT);
  
  // ADC設定の改善
  analogSetAttenuation(ADC_11db);  // 3.9V範囲
  analogSetWidth(12);              // 12bit解像度
  
  // ペダル校正初期化
  calibrator.begin();
  
  // BLE-MIDI接続/切断コールバック
  BLEMIDI.setHandleConnected([]() {
    isConnected = true;
    displayNeedsUpdate = true;  // 画面更新フラグ
    Serial.println("BLE-MIDI Connected (NimBLE)");
    if (!calibrator.isCalibrating()) {
      updateMainDisplay();
    }
  });
  
  BLEMIDI.setHandleDisconnected([]() {
    isConnected = false;
    displayNeedsUpdate = true;  // 画面更新フラグ
    Serial.println("BLE-MIDI Disconnected (NimBLE)");
    Serial.println("Waiting for reconnection...");
    
    if (!calibrator.isCalibrating()) {
      updateMainDisplay();
    }
    
    // 注意：MIDI.begin()を再実行しない
    // アドバタイジングは自動的に継続される
  });
  
  // BLE-MIDIサービス開始（NimBLEバックエンド使用）
  MIDI.begin();
  
  Serial.println("BLE-MIDI Controller Ready (NimBLE backend)");
  
  // 初期化完了表示
  delay(1000);
  updateMainDisplay();
  
  // 起動時にバッテリー測定開始
  batteryInitialized = true;
  // 起動時は即座に1回測定
  currentBatteryVoltage = M5.Power.getBatteryVoltage();
  m5BatteryLevel = M5.Power.getBatteryLevel();
  batteryPercent = calculateBatteryPercent(currentBatteryVoltage);
  drawBattery();
}

void loop() {
  M5.update();
  
  // バッテリー測定（非同期・MIDI処理を妨げない）
  updateBatteryAsync();
  
  // 定期的なバッテリー表示更新（測定完了後または1分経過後）
  unsigned long currentTime = millis();
  if (batteryMeasurementCount == 0 && (currentTime - lastBatteryCheck > BATTERY_CHECK_INTERVAL)) {
    // 1分経過したが測定が完了していない場合、測定を開始
    lastBatteryMeasurement = 0; // 次回のupdateBatteryAsync()で即座に測定開始
  }
  
  // バッテリー値が更新された場合、表示を更新
  static int lastDisplayedBattery = -1;
  if (batteryPercent != lastDisplayedBattery) {
    drawBattery();
    lastDisplayedBattery = batteryPercent;
  }
  
  // 定期的な接続状態チェック
  if (currentTime - lastConnectionCheck > CONNECTION_CHECK_INTERVAL) {
    lastConnectionCheck = currentTime;
  }
  
  // アナログ値読み取り (0-4095)
  int analogValue = analogRead(ANALOG_PIN);
  
  // ボタンBで校正開始
  if (M5.BtnB.wasPressed()) {
    calibrator.startCalibration();
  }
  
  // 校正モード中の処理
  if (calibrator.isCalibrating()) {
    calibrator.updateCalibration(analogValue);
    return;  // 校正中はMIDI送信しない
  }
  
  // 校正済みのMIDI値取得
  int midiValue = calibrator.getMidiValue(analogValue);
  
  // ノイズフィルタリング
  if (abs(midiValue - lastValue) > THRESHOLD) {
    // MIDI CC送信（接続時のみ）
    if (isConnected) {
      MIDI.sendControlChange(CC_NUMBER, midiValue, MIDI_CHANNEL);
      Serial.printf("MIDI Sent - Analog: %d, CC: %d\n", analogValue, midiValue);
    } else {
      Serial.printf("Analog: %d, MIDI: %d (not connected)\n", analogValue, midiValue);
    }
    
    // 値の表示エリアをクリア（バッテリー表示は保持）
    M5.Display.fillRect(0, 28, SCREEN_WIDTH-30, 35, BLACK);
    
    // アナログ値とMIDI値を表示（コンパクトに）
    M5.Display.drawString("ADC: " + String(analogValue), 5, 28);
    M5.Display.drawString("CC" + String(CC_NUMBER) + ": " + String(midiValue), 5, 40);
    
    // MIDI値のバーグラフ表示
    int barWidth = (midiValue * 100) / 127;  // 0-100の範囲
    M5.Display.drawRect(5, 52, 102, 8, WHITE);
    if (barWidth > 0) {
      M5.Display.fillRect(6, 53, barWidth, 6, GREEN);
    }
    // バーの余白をクリア
    if (barWidth < 100) {
      M5.Display.fillRect(6 + barWidth, 53, 100 - barWidth, 6, BLACK);
    }
    
    lastValue = midiValue;
  }
  
  // ボタンA処理例
  if (M5.BtnA.wasPressed()) {
    // ボタンAでノートオン/オフ（接続時のみ）
    if (isConnected) {
      static bool noteOn = false;
      if (!noteOn) {
        MIDI.sendNoteOn(60, 127, MIDI_CHANNEL);  // C4
        noteOn = true;
        Serial.println("Note On sent");
      } else {
        MIDI.sendNoteOff(60, 0, MIDI_CHANNEL);
        noteOn = false;
        Serial.println("Note Off sent");
      }
    } else {
      Serial.println("Button pressed, but not connected");
    }
  }
  
  delay(10);  // 100Hz更新
}