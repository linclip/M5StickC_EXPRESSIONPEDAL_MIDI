// 必要なライブラリをインクルードします。
// ArduinoIDEのライブラリマネージャからlathoub/Arduino-BLE-MIDIをインストールしてください。
// 最新版ではBLEMIDI_Transport.hとBLEMIDI_ESP32_NimBLE.hをインクルードします。
#include <M5Unified.h>
#include <BLEMIDI_Transport.h>
#include <hardware/BLEMIDI_ESP32_NimBLE.h>

// MIDIチャンネルとコントローラー番号を変数として定義します。
// これにより、プログラムの変更が容易になります。
#define MIDI_CHANNEL 1
#define CC_NUMBER 7

// BLE-MIDIのインスタンスをマクロを使って作成します。
// このマクロは、自動的にbleMidi_instanceという名前のインスタンスを生成します。
BLEMIDI_CREATE_DEFAULT_INSTANCE()

// アナログ信号を読み取るピンを指定します。
// M5StickCのHATピンはG26とG36です。G36はADC（アナログ-デジタル変換）に特に適しています。
const int analogPin = 36;

// 前回のMIDI送信値とアナログ値を保持する変数
int lastMidiValue = -1;
int lastAnalogValue = -1;

bool isConnected = false;


void setup() {
  // M5Unifiedの初期化
  // この呼び出しだけで、自動的にデバイスを判別して初期化してくれます。
  M5.begin();


  // BLE-MIDIの初期化
  // マクロで作成したインスタンス名「bleMidi_instance」を使って初期化します。
  MIDI.begin();

  pinMode(analogPin, INPUT);
  BLEMIDI.setHandleConnected([]()
                             {
                               Serial.println("---------CONNECTED---------");
                               isConnected = true;
                               //digitalWrite(LED_BUILTIN, HIGH);
                             });

  BLEMIDI.setHandleDisconnected([]()
                                {
                                  Serial.println("---------NOT CONNECTED---------");
                                  isConnected = false;
                                  //digitalWrite(LED_BUILTIN, LOW);
                                });

}

void loop() {
  // BLE-MIDIのサービスが動作しているか確認
  if (isConnected) {
    // アナログピンから値を読み取る（0〜4095の範囲）
    // G36はADC1に対応しており、自動的に12ビットの分解能で読み込まれます。
    //int analogValue = M5.In_A.getRawValue(analogPin); // ● In_Aなんてない

    int analogValue = analogRead(analogPin);

    // ノイズ対策のため、値が大きく変化した場合のみ処理を行います。
    // 5未満の変化は無視します。
    if (abs(analogValue - lastAnalogValue) > 5) {
      // アナログ値をMIDIのコントロールチェンジ値（0〜127）に変換
      // map()関数を使って範囲を変換します。
      int midiValue = map(analogValue, 0, 4095, 0, 127);
      
      // 前回のMIDI送信値と異なる場合のみ送信
      // これにより、不必要なMIDIメッセージの送信を減らします。
      if (midiValue != lastMidiValue) {
        // MIDIコントロールチェンジメッセージを送信
        // 定義した変数を使ってチャンネルとコントローラー番号を指定します。
        //MIDI.controlChange(MIDI_CHANNEL, CC_NUMBER, midiValue);

        MIDI.sendControlChange(MIDI_CHANNEL, CC_NUMBER, midiValue);

        // シリアルモニターに送信したMIDI値を出力（デバッグ用）
        // Serial.println(midiValue);
        
        lastMidiValue = midiValue;
      }
      lastAnalogValue = analogValue;
    }
  }

  // ループの最後にM5Unifiedの更新を呼び出し
  // ボタン状態やセンサー値の更新に必要です。
  M5.update();

  // ループの速度を調整
  // これにより、CPU負荷を下げ、不必要なMIDIメッセージの送信を防ぎます。
  delay(10);
}
