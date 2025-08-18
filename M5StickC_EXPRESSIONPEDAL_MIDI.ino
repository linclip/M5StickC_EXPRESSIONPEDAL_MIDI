/*
Wireless Expression Pedal HAT for M5StickC

Board Manager: esp32 by Espressif Systems v2.0.17 - M5Stick-C
Libraries:
  M5Unified v0.2.5
  NimBLE-Arduino v1.4.3
  BLE-MIDI by lathoub v2.2
  MIDI Library by lathoub v5.0.2
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

// アナログ値のフィルタリング用
int lastValue = 0;
const int THRESHOLD = 5;  // ノイズ除去のための閾値

// BLE接続状態管理
bool isConnected = false;
bool displayNeedsUpdate = true;  // 画面更新フラグ
unsigned long lastConnectionCheck = 0;
const unsigned long CONNECTION_CHECK_INTERVAL = 1000; // 1秒ごとにチェック

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
        minVal = 4095;  // リセット
        maxVal = 0;
        Serial.println("Calibration started - move pedal fully for 5 seconds!");
        
        // 画面表示
        M5.Display.clear();
        M5.Display.drawString("Calibration Mode", 10, 10);
        M5.Display.drawString("Move pedal fully!", 10, 30);
    }
    
    bool updateCalibration(int rawValue) {
        if (!calibrationMode) return false;
        
        // 範囲を更新
        minVal = min(minVal, rawValue);
        maxVal = max(maxVal, rawValue);
        
        // 進捗表示
        unsigned long elapsed = millis() - calibrationStart;
        int progress = (elapsed * 100) / CALIBRATION_TIME;
        M5.Display.fillRect(0, 50, 160, 40, BLACK);
        M5.Display.drawString("Progress: " + String(progress) + "%", 10, 50);
        M5.Display.drawString("Min:" + String(minVal), 10, 65);
        M5.Display.drawString("Max:" + String(maxVal), 10, 80);
        
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
        M5.Display.drawString("Calibration Done!", 10, 10);
        M5.Display.drawString("Min: " + String(minVal), 10, 30);
        M5.Display.drawString("Max: " + String(maxVal), 10, 50);
        delay(2000);  // 2秒間表示
        
        // 通常画面に戻す
        M5.Display.clear();
        M5.Display.drawString("M5EXP-MIDI (NimBLE)", 5, 10);
        M5.Display.drawString(isConnected ? "Connected!" : "Disconnected", 10, 30);
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

void setup() {
  // M5StickC初期化
  auto cfg = M5.config();
  M5.begin(cfg);
  
  M5.Display.setRotation(1);
  M5.Display.setTextSize(1);
  M5.Display.clear();
  M5.Display.drawString("M5EXP-MIDI (NimBLE)", 5, 10);
  M5.Display.drawString("Initializing...", 10, 30);
  
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
      M5.Display.clear();
      M5.Display.drawString("M5EXP-MIDI (NimBLE)", 5, 10);
      M5.Display.drawString("Connected!", 10, 30);
    }
  });
  
  BLEMIDI.setHandleDisconnected([]() {
    isConnected = false;
    displayNeedsUpdate = true;  // 画面更新フラグ
    Serial.println("BLE-MIDI Disconnected (NimBLE)");
    Serial.println("Waiting for reconnection...");
    
    if (!calibrator.isCalibrating()) {
      M5.Display.clear();
      M5.Display.drawString("M5EXP-MIDI (NimBLE)", 5, 10);
      M5.Display.drawString("Disconnected", 10, 30);
    }
    
    // 注意：MIDI.begin()を再実行しない
    // アドバタイジングは自動的に継続される
  });
  
  // BLE-MIDIサービス開始（NimBLEバックエンド使用）
  MIDI.begin();
  
  Serial.println("BLE-MIDI Controller Ready (NimBLE backend)");
  M5.Display.drawString("Ready (NimBLE)", 10, 45);
  M5.Display.drawString("BtnB: Calibrate", 10, 60);
}

void loop() {
  M5.update();
  
  // 定期的な接続状態チェック
  unsigned long currentTime = millis();
  if (currentTime - lastConnectionCheck > CONNECTION_CHECK_INTERVAL) {
    lastConnectionCheck = currentTime;
    
    // BLE接続状態を強制チェック（デバッグ用）
    // 実際の接続状態とisConnectedが一致しない場合の対策
    static int disconnectCounter = 0;
    if (isConnected) {
      // 接続中だが実際には切断されている可能性をチェック
      // 何らかの方法で実際の接続状態を確認したい場合はここに実装
    }
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
    
    // ディスプレイ更新（元の方式：常時更新で確実な表示）
    M5.Display.fillRect(0, 70, 160, 40, BLACK);
    M5.Display.drawString("Status: " + String(isConnected ? "Connected" : "Waiting"), 5, 70);
    M5.Display.drawString("Analog: " + String(analogValue), 10, 85);
    M5.Display.drawString("MIDI: " + String(midiValue), 10, 100);
    
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