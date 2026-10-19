// TurboWarp Extension: Web Bluetooth (BLE) フル機能版
// Part of "michibit the second" (M5Stack Global Innovation Contest 2026)
// Author: fumi24 (+ Claude)
// License: MIT (this file)
//
// 使い方: TurboWarpの「カスタム拡張機能を読み込む」で「ファイルから読み込む」を選び、
//        このファイルを選択してください。読み込み時に「サンドボックス化しない」を
//        必ずONにしてください（Web Bluetooth APIに直接アクセスするため必須です）。
//
// 重要: Web Bluetooth APIはHTTPS環境(またはlocalhost)でのみ動作し、対応ブラウザは
//       Chrome/Edge/Opera系のみです（Firefox/Safariは非対応）。
//       requestDevice()系のブロックはユーザーの明示的な操作（クリック等）を起点として
//       実行しないとブラウザ側でブロックされる場合があります。

(function (Scratch) {
  'use strict';

  if (!Scratch.extensions.unsandboxed) {
    throw new Error('このBLE拡張機能は「サンドボックス化しない」設定で読み込む必要があります。');
  }

  // ---------------------------------------------------------------------
  // 共通GATTサービス／キャラクタリスティックのUUID一覧（よく使うもの）
  // ---------------------------------------------------------------------
  const SERVICE_UUIDS = {
    'ジェネリックアクセス (1800)': 'generic_access',
    'ジェネリックアトリビュート (1801)': 'generic_attribute',
    'デバイス情報 (180A)': 'device_information',
    'バッテリーサービス (180F)': 'battery_service',
    '心拍数 (180D)': 'heart_rate',
    '体温計 (1809)': 'health_thermometer',
    '環境センシング (181A)': 'environmental_sensing',
    '現在時刻 (1805)': 'current_time',
    'ヒューマンインターフェースデバイス (1812)': 'human_interface_device',
    'イミディエイトアラート (1802)': 'immediate_alert',
    'リンクロス (1803)': 'link_loss',
    '送信電力 (1804)': 'tx_power'
  };

  const CHARACTERISTIC_UUIDS = {
    'デバイス名 (2A00)': 'device_name',
    'バッテリー残量 (2A19)': 'battery_level',
    'メーカー名 (2A29)': 'manufacturer_name_string',
    '型番 (2A24)': 'model_number_string',
    'シリアル番号 (2A25)': 'serial_number_string',
    'ファームウェア版数 (2A26)': 'firmware_revision_string',
    'ハードウェア版数 (2A27)': 'hardware_revision_string',
    'ソフトウェア版数 (2A28)': 'software_revision_string',
    '心拍数測定値 (2A37)': 'heart_rate_measurement',
    '温度 (2A6E)': 'temperature',
    '湿度 (2A6F)': 'humidity',
    '気圧 (2A6D)': 'pressure',
    '現在時刻 (2A2B)': 'current_time'
  };

  const VALUE_FORMATS = {
    'テキスト(UTF-8)': 'text',
    '16進数(hex)': 'hex',
    '10進バイト列(カンマ区切り)': 'bytes',
    '符号なし8bit整数': 'uint8',
    '符号あり8bit整数': 'int8',
    '符号なし16bit整数(LE)': 'uint16le',
    '符号あり16bit整数(LE)': 'int16le',
    '符号なし32bit整数(LE)': 'uint32le',
    '符号あり32bit整数(LE)': 'int32le',
    '32bit浮動小数点(LE)': 'float32le'
  };

  const WRITE_FORMATS = {
    'テキスト(UTF-8)': 'text',
    '16進数(hex)': 'hex',
    '10進バイト列(カンマ区切り)': 'bytes'
  };

  const RESPONSE_TYPES = {
    '応答あり (Write With Response)': 'with',
    '応答なし (Write Without Response)': 'without'
  };

  // ---------------------------------------------------------------------
  // エンコード／デコード ヘルパー
  // ---------------------------------------------------------------------
  function encodeToBytes(value, format) {
    value = Scratch.Cast.toString(value);
    switch (format) {
      case 'text':
        return new TextEncoder().encode(value);
      case 'hex': {
        const clean = value.replace(/[^0-9a-fA-F]/g, '');
        const bytes = new Uint8Array(Math.floor(clean.length / 2));
        for (let i = 0; i < bytes.length; i++) {
          bytes[i] = parseInt(clean.substr(i * 2, 2), 16);
        }
        return bytes;
      }
      case 'bytes': {
        const parts = value.split(',').map(s => s.trim()).filter(s => s.length > 0);
        return new Uint8Array(parts.map(n => parseInt(n, 10) & 0xff));
      }
      default:
        return new TextEncoder().encode(value);
    }
  }

  function decodeFromDataView(dataView, format) {
    if (!dataView) return '';
    switch (format) {
      case 'text':
        return new TextDecoder('utf-8').decode(dataView.buffer.slice(dataView.byteOffset, dataView.byteOffset + dataView.byteLength));
      case 'hex': {
        let s = '';
        for (let i = 0; i < dataView.byteLength; i++) {
          s += dataView.getUint8(i).toString(16).padStart(2, '0');
        }
        return s;
      }
      case 'bytes': {
        const arr = [];
        for (let i = 0; i < dataView.byteLength; i++) arr.push(dataView.getUint8(i));
        return arr.join(',');
      }
      case 'uint8':
        return dataView.byteLength >= 1 ? String(dataView.getUint8(0)) : '';
      case 'int8':
        return dataView.byteLength >= 1 ? String(dataView.getInt8(0)) : '';
      case 'uint16le':
        return dataView.byteLength >= 2 ? String(dataView.getUint16(0, true)) : '';
      case 'int16le':
        return dataView.byteLength >= 2 ? String(dataView.getInt16(0, true)) : '';
      case 'uint32le':
        return dataView.byteLength >= 4 ? String(dataView.getUint32(0, true)) : '';
      case 'int32le':
        return dataView.byteLength >= 4 ? String(dataView.getInt32(0, true)) : '';
      case 'float32le':
        return dataView.byteLength >= 4 ? String(dataView.getFloat32(0, true)) : '';
      default:
        return '';
    }
  }

  // ---------------------------------------------------------------------
  // 拡張機能本体
  // ---------------------------------------------------------------------
  class BLEExtension {
    constructor(runtime) {
      this.runtime = runtime;

      this.device = null;
      this.server = null;
      this.services = new Map();          // uuid -> BluetoothRemoteGATTService
      this.characteristics = new Map();   // uuid -> BluetoothRemoteGATTCharacteristic
      this.descriptors = new Map();       // "charUuid|descUuid" -> BluetoothRemoteGATTDescriptor

      this.lastReadValue = null;          // 直近read()した値 (DataView)
      this.notifyLatest = new Map();      // charUuid -> DataView (直近の通知値)
      this.notifyFlag = new Map();        // charUuid -> boolean (新着フラグ, hat用)
      this.notifyLatestUuid = '';         // 最後に通知が来たcharUuidをまとめて拾う用

      this.disconnectedFlag = false;      // 切断エッジ検出用
      this.errorMessage = '';

      this.scanning = false;
      this.scanResults = [];              // {name, id, rssi, uuids, mfgData}
      this.newAdvertisementFlag = false;
      this.lastAdvertisement = null;

      this.knownDevices = [];             // getDevices()結果キャッシュ

      this.lastRequestOptions = '';       // デバッグ: 直前にrequestDeviceへ渡したoptions
    }

    getInfo() {
      return {
        id: 'ble',
        name: 'BLE (Web Bluetooth)',
        color1: '#0070BB',
        color2: '#005A96',
        blocks: [
          { blockType: Scratch.BlockType.LABEL, text: '接続の管理' },
          {
            opcode: 'isSupported',
            blockType: Scratch.BlockType.BOOLEAN,
            text: 'このブラウザはWeb Bluetoothに対応している'
          },
          {
            opcode: 'requestAndConnect',
            blockType: Scratch.BlockType.COMMAND,
            text: 'デバイスを検索して接続する 条件:[FILTERTYPE] 値:[FILTERVALUE] 使う可能性のあるサービス:[OPTSERVICES]',
            arguments: {
              FILTERTYPE: { type: Scratch.ArgumentType.STRING, menu: 'filterType', defaultValue: 'すべてのデバイスを表示' },
              FILTERVALUE: { type: Scratch.ArgumentType.STRING, defaultValue: '' },
              OPTSERVICES: { type: Scratch.ArgumentType.STRING, defaultValue: 'battery_service' }
            }
          },
          {
            opcode: 'connectGatt',
            blockType: Scratch.BlockType.COMMAND,
            text: 'GATTサーバーに接続する（再接続）'
          },
          {
            opcode: 'disconnect',
            blockType: Scratch.BlockType.COMMAND,
            text: '切断する'
          },
          {
            opcode: 'isConnected',
            blockType: Scratch.BlockType.BOOLEAN,
            text: '接続されている'
          },
          {
            opcode: 'whenDisconnected',
            blockType: Scratch.BlockType.HAT,
            text: '切断されたとき',
            isEdgeActivated: true
          },
          {
            opcode: 'deviceName',
            blockType: Scratch.BlockType.REPORTER,
            text: 'デバイス名'
          },
          {
            opcode: 'deviceId',
            blockType: Scratch.BlockType.REPORTER,
            text: 'デバイスID'
          },
          {
            opcode: 'lastError',
            blockType: Scratch.BlockType.REPORTER,
            text: '最後のエラーメッセージ'
          },
          {
            opcode: 'lastRequestOptionsDebug',
            blockType: Scratch.BlockType.REPORTER,
            text: '【デバッグ】直前の検索条件(ブラウザに渡した内容)'
          },

          '---',
          { blockType: Scratch.BlockType.LABEL, text: '許可済みデバイス' },
          {
            opcode: 'refreshKnownDevices',
            blockType: Scratch.BlockType.COMMAND,
            text: '許可済みデバイス一覧を更新する（getDevices）'
          },
          {
            opcode: 'knownDeviceCount',
            blockType: Scratch.BlockType.REPORTER,
            text: '許可済みデバイスの数'
          },
          {
            opcode: 'knownDeviceNameAt',
            blockType: Scratch.BlockType.REPORTER,
            text: '許可済みデバイス [INDEX] 番目の名前',
            arguments: { INDEX: { type: Scratch.ArgumentType.NUMBER, defaultValue: 1 } }
          },
          {
            opcode: 'connectKnownDeviceAt',
            blockType: Scratch.BlockType.COMMAND,
            text: '許可済みデバイス [INDEX] 番目に接続する',
            arguments: { INDEX: { type: Scratch.ArgumentType.NUMBER, defaultValue: 1 } }
          },

          '---',
          { blockType: Scratch.BlockType.LABEL, text: 'サービス／キャラクタリスティック取得' },
          {
            opcode: 'getService',
            blockType: Scratch.BlockType.COMMAND,
            text: 'サービス [UUID] を取得する',
            arguments: { UUID: { type: Scratch.ArgumentType.STRING, defaultValue: 'battery_service' } }
          },
          {
            opcode: 'hasService',
            blockType: Scratch.BlockType.BOOLEAN,
            text: 'サービス [UUID] を取得済み',
            arguments: { UUID: { type: Scratch.ArgumentType.STRING, defaultValue: 'battery_service' } }
          },
          {
            opcode: 'getCharacteristic',
            blockType: Scratch.BlockType.COMMAND,
            text: 'サービス [SUUID] からキャラクタリスティック [CUUID] を取得する',
            arguments: {
              SUUID: { type: Scratch.ArgumentType.STRING, defaultValue: 'battery_service' },
              CUUID: { type: Scratch.ArgumentType.STRING, defaultValue: 'battery_level' }
            }
          },
          {
            opcode: 'hasCharacteristic',
            blockType: Scratch.BlockType.BOOLEAN,
            text: 'キャラクタリスティック [CUUID] を取得済み',
            arguments: { CUUID: { type: Scratch.ArgumentType.STRING, defaultValue: 'battery_level' } }
          },

          '---',
          { blockType: Scratch.BlockType.LABEL, text: '読み書き' },
          {
            opcode: 'readCharacteristic',
            blockType: Scratch.BlockType.COMMAND,
            text: 'キャラクタリスティック [CUUID] の値を読み込む',
            arguments: { CUUID: { type: Scratch.ArgumentType.STRING, defaultValue: 'battery_level' } }
          },
          {
            opcode: 'lastReadValueAs',
            blockType: Scratch.BlockType.REPORTER,
            text: '読み込んだ値を [FORMAT] として取得',
            arguments: { FORMAT: { type: Scratch.ArgumentType.STRING, menu: 'valueFormat', defaultValue: 'テキスト(UTF-8)' } }
          },
          {
            opcode: 'writeCharacteristic',
            blockType: Scratch.BlockType.COMMAND,
            text: 'キャラクタリスティック [CUUID] に [VALUE] を [FORMAT] として [RESPONSE] で書き込む',
            arguments: {
              CUUID: { type: Scratch.ArgumentType.STRING, defaultValue: 'battery_level' },
              VALUE: { type: Scratch.ArgumentType.STRING, defaultValue: '1' },
              FORMAT: { type: Scratch.ArgumentType.STRING, menu: 'writeFormat', defaultValue: 'テキスト(UTF-8)' },
              RESPONSE: { type: Scratch.ArgumentType.STRING, menu: 'responseType', defaultValue: '応答あり (Write With Response)' }
            }
          },

          '---',
          { blockType: Scratch.BlockType.LABEL, text: '通知(Notify)' },
          {
            opcode: 'startNotifications',
            blockType: Scratch.BlockType.COMMAND,
            text: 'キャラクタリスティック [CUUID] の通知を開始する',
            arguments: { CUUID: { type: Scratch.ArgumentType.STRING, defaultValue: 'battery_level' } }
          },
          {
            opcode: 'stopNotifications',
            blockType: Scratch.BlockType.COMMAND,
            text: 'キャラクタリスティック [CUUID] の通知を停止する',
            arguments: { CUUID: { type: Scratch.ArgumentType.STRING, defaultValue: 'battery_level' } }
          },
          {
            opcode: 'whenNotified',
            blockType: Scratch.BlockType.HAT,
            text: 'キャラクタリスティック [CUUID] から通知を受信したとき',
            isEdgeActivated: true,
            arguments: { CUUID: { type: Scratch.ArgumentType.STRING, defaultValue: 'battery_level' } }
          },
          {
            opcode: 'lastNotifiedUuid',
            blockType: Scratch.BlockType.REPORTER,
            text: '最後に通知してきたキャラクタリスティックのUUID'
          },
          {
            opcode: 'notifiedValueAs',
            blockType: Scratch.BlockType.REPORTER,
            text: 'キャラクタリスティック [CUUID] の最新の通知値を [FORMAT] として取得',
            arguments: {
              CUUID: { type: Scratch.ArgumentType.STRING, defaultValue: 'battery_level' },
              FORMAT: { type: Scratch.ArgumentType.STRING, menu: 'valueFormat', defaultValue: 'テキスト(UTF-8)' }
            }
          },

          '---',
          { blockType: Scratch.BlockType.LABEL, text: 'ディスクリプタ' },
          {
            opcode: 'getDescriptor',
            blockType: Scratch.BlockType.COMMAND,
            text: 'キャラクタリスティック [CUUID] からディスクリプタ [DUUID] を取得する',
            arguments: {
              CUUID: { type: Scratch.ArgumentType.STRING, defaultValue: 'battery_level' },
              DUUID: { type: Scratch.ArgumentType.STRING, defaultValue: '00002901-0000-1000-8000-00805f9b34fb' }
            }
          },
          {
            opcode: 'readDescriptor',
            blockType: Scratch.BlockType.COMMAND,
            text: 'ディスクリプタ [DUUID] (親:[CUUID]) の値を読み込む',
            arguments: {
              CUUID: { type: Scratch.ArgumentType.STRING, defaultValue: 'battery_level' },
              DUUID: { type: Scratch.ArgumentType.STRING, defaultValue: '00002901-0000-1000-8000-00805f9b34fb' }
            }
          },
          {
            opcode: 'lastDescriptorValueAs',
            blockType: Scratch.BlockType.REPORTER,
            text: '読み込んだディスクリプタの値を [FORMAT] として取得',
            arguments: { FORMAT: { type: Scratch.ArgumentType.STRING, menu: 'valueFormat', defaultValue: 'テキスト(UTF-8)' } }
          },
          {
            opcode: 'writeDescriptor',
            blockType: Scratch.BlockType.COMMAND,
            text: 'ディスクリプタ [DUUID] (親:[CUUID]) に [VALUE] を [FORMAT] として書き込む',
            arguments: {
              CUUID: { type: Scratch.ArgumentType.STRING, defaultValue: 'battery_level' },
              DUUID: { type: Scratch.ArgumentType.STRING, defaultValue: '00002901-0000-1000-8000-00805f9b34fb' },
              VALUE: { type: Scratch.ArgumentType.STRING, defaultValue: '' },
              FORMAT: { type: Scratch.ArgumentType.STRING, menu: 'writeFormat', defaultValue: 'テキスト(UTF-8)' }
            }
          },

          '---',
          { blockType: Scratch.BlockType.LABEL, text: 'スキャン／アドバタイズ（実験的機能・要フラグ有効化）' },
          {
            opcode: 'startScan',
            blockType: Scratch.BlockType.COMMAND,
            text: '周囲のBLEデバイスのスキャンを開始する（LE Scan）'
          },
          {
            opcode: 'stopScan',
            blockType: Scratch.BlockType.COMMAND,
            text: 'スキャンを停止する'
          },
          {
            opcode: 'isScanning',
            blockType: Scratch.BlockType.BOOLEAN,
            text: 'スキャン中'
          },
          {
            opcode: 'whenAdvertisementReceived',
            blockType: Scratch.BlockType.HAT,
            text: '新しいアドバタイズを受信したとき',
            isEdgeActivated: true
          },
          {
            opcode: 'lastAdvName',
            blockType: Scratch.BlockType.REPORTER,
            text: '受信したアドバタイズのデバイス名'
          },
          {
            opcode: 'lastAdvRssi',
            blockType: Scratch.BlockType.REPORTER,
            text: '受信したアドバタイズのRSSI'
          },
          {
            opcode: 'lastAdvId',
            blockType: Scratch.BlockType.REPORTER,
            text: '受信したアドバタイズのデバイスID'
          },
          {
            opcode: 'watchAdvertisements',
            blockType: Scratch.BlockType.COMMAND,
            text: '接続中デバイスのアドバタイズ監視を開始する（watchAdvertisements）'
          },

          '---',
          { blockType: Scratch.BlockType.LABEL, text: 'UUIDユーティリティ' },
          {
            opcode: 'commonServiceUuid',
            blockType: Scratch.BlockType.REPORTER,
            text: '共通サービス: [SVC]',
            arguments: { SVC: { type: Scratch.ArgumentType.STRING, menu: 'commonService', defaultValue: 'バッテリーサービス (180F)' } }
          },
          {
            opcode: 'commonCharacteristicUuid',
            blockType: Scratch.BlockType.REPORTER,
            text: '共通キャラクタリスティック: [CHR]',
            arguments: { CHR: { type: Scratch.ArgumentType.STRING, menu: 'commonCharacteristic', defaultValue: 'バッテリー残量 (2A19)' } }
          },
          {
            opcode: 'canonicalUuid',
            blockType: Scratch.BlockType.REPORTER,
            text: '[UUID] を正式なUUID形式に変換する',
            arguments: { UUID: { type: Scratch.ArgumentType.STRING, defaultValue: '180f' } }
          }
        ],
        menus: {
          filterType: {
            acceptReporters: true,
            items: ['すべてのデバイスを表示', '名前で絞り込み(完全一致)', '名前の一部で絞り込み', 'サービスUUIDで絞り込み']
          },
          valueFormat: { acceptReporters: true, items: Object.keys(VALUE_FORMATS) },
          writeFormat: { acceptReporters: true, items: Object.keys(WRITE_FORMATS) },
          responseType: { acceptReporters: true, items: Object.keys(RESPONSE_TYPES) },
          commonService: { acceptReporters: true, items: Object.keys(SERVICE_UUIDS) },
          commonCharacteristic: { acceptReporters: true, items: Object.keys(CHARACTERISTIC_UUIDS) }
        }
      };
    }

    // ---------------- 基本サポート確認 ----------------
    isSupported() {
      return typeof navigator !== 'undefined' && !!navigator.bluetooth;
    }

    // ---------------- 接続 ----------------
    async requestAndConnect(args) {
      if (!this.isSupported()) {
        this.errorMessage = 'このブラウザはWeb Bluetoothに対応していません';
        return;
      }
      // 前後の空白混入によるミス一致を防ぐためtrim
      const filterType = Scratch.Cast.toString(args.FILTERTYPE).trim();
      const filterValue = Scratch.Cast.toString(args.FILTERVALUE).trim();
      const optServices = Scratch.Cast.toString(args.OPTSERVICES)
        .split(',').map(s => s.trim()).filter(s => s.length > 0);

      const options = {};
      let matched = true;
      if (filterType === 'すべてのデバイスを表示') {
        options.acceptAllDevices = true;
      } else if (filterType === '名前で絞り込み(完全一致)') {
        options.filters = [{ name: filterValue }];
      } else if (filterType === '名前の一部で絞り込み') {
        options.filters = [{ namePrefix: filterValue }];
      } else if (filterType === 'サービスUUIDで絞り込み') {
        options.filters = [{ services: [filterValue] }];
      } else {
        // どのケースにも一致しない場合(バグの疑いが強い)。
        // 安全側に倒して全デバイス表示にするが、デバッグブロックで検知できるようフラグを立てる。
        matched = false;
        options.acceptAllDevices = true;
      }
      if (optServices.length > 0) {
        options.optionalServices = optServices;
      }

      // デバッグ用: 実際にrequestDeviceへ渡す直前のoptionsをそのまま保存
      this.lastRequestOptions = JSON.stringify({
        receivedFilterType: filterType,
        matchedKnownCase: matched,
        optionsSentToBrowser: options
      });

      try {
        const device = await navigator.bluetooth.requestDevice(options);
        this._bindDevice(device);
        await this.connectGatt();
      } catch (e) {
        this.errorMessage = String(e && e.message ? e.message : e);
      }
    }

    lastRequestOptionsDebug() {
      return this.lastRequestOptions;
    }

    _bindDevice(device) {
      // 前のデバイスのリスナーを外す
      if (this.device) {
        this.device.removeEventListener('gattserverdisconnected', this._onDisconnected);
        this.device.removeEventListener('advertisementreceived', this._onAdvertisement);
      }
      this.device = device;
      this.server = null;
      this.services.clear();
      this.characteristics.clear();
      this.descriptors.clear();

      this._onDisconnected = () => {
        this.disconnectedFlag = true;
        this.server = null;
      };
      this._onAdvertisement = (evt) => {
        this.newAdvertisementFlag = true;
        this.lastAdvertisement = {
          name: evt.device && evt.device.name,
          id: evt.device && evt.device.id,
          rssi: evt.rssi
        };
      };
      device.addEventListener('gattserverdisconnected', this._onDisconnected);
      device.addEventListener('advertisementreceived', this._onAdvertisement);
    }

    async connectGatt() {
      if (!this.device) {
        this.errorMessage = 'デバイスが選択されていません（先に検索して接続するブロックを使ってください）';
        return;
      }
      try {
        this.server = await this.device.gatt.connect();
        this.disconnectedFlag = false;
      } catch (e) {
        this.errorMessage = String(e && e.message ? e.message : e);
      }
    }

    disconnect() {
      if (this.device && this.device.gatt && this.device.gatt.connected) {
        this.device.gatt.disconnect();
      }
    }

    isConnected() {
      return !!(this.device && this.device.gatt && this.device.gatt.connected);
    }

    whenDisconnected() {
      if (this.disconnectedFlag) {
        return true;
      }
      return false;
    }

    deviceName() {
      return this.device ? (this.device.name || '') : '';
    }

    deviceId() {
      return this.device ? (this.device.id || '') : '';
    }

    lastError() {
      return this.errorMessage;
    }

    // ---------------- 許可済みデバイス ----------------
    async refreshKnownDevices() {
      try {
        if (navigator.bluetooth.getDevices) {
          this.knownDevices = await navigator.bluetooth.getDevices();
        } else {
          this.errorMessage = 'このブラウザは getDevices() に対応していません';
          this.knownDevices = [];
        }
      } catch (e) {
        this.errorMessage = String(e && e.message ? e.message : e);
      }
    }

    knownDeviceCount() {
      return this.knownDevices.length;
    }

    knownDeviceNameAt(args) {
      const i = Math.round(Scratch.Cast.toNumber(args.INDEX)) - 1;
      const d = this.knownDevices[i];
      return d ? (d.name || '(名称なし)') : '';
    }

    async connectKnownDeviceAt(args) {
      const i = Math.round(Scratch.Cast.toNumber(args.INDEX)) - 1;
      const d = this.knownDevices[i];
      if (!d) {
        this.errorMessage = '指定した番号のデバイスがありません。先に一覧を更新してください';
        return;
      }
      this._bindDevice(d);
      await this.connectGatt();
    }

    // ---------------- サービス／キャラクタリスティック ----------------
    async getService(args) {
      if (!this.server) { this.errorMessage = 'GATTサーバーに接続していません'; return; }
      const uuid = Scratch.Cast.toString(args.UUID);
      try {
        const service = await this.server.getPrimaryService(uuid);
        this.services.set(uuid, service);
      } catch (e) {
        this.errorMessage = String(e && e.message ? e.message : e);
      }
    }

    hasService(args) {
      return this.services.has(Scratch.Cast.toString(args.UUID));
    }

    async getCharacteristic(args) {
      const suuid = Scratch.Cast.toString(args.SUUID);
      const cuuid = Scratch.Cast.toString(args.CUUID);
      let service = this.services.get(suuid);
      try {
        if (!service) {
          if (!this.server) { this.errorMessage = 'GATTサーバーに接続していません'; return; }
          service = await this.server.getPrimaryService(suuid);
          this.services.set(suuid, service);
        }
        const characteristic = await service.getCharacteristic(cuuid);
        this.characteristics.set(cuuid, characteristic);

        characteristic.removeEventListener('characteristicvaluechanged', characteristic._bleExtHandler || (() => {}));
        const handler = (evt) => {
          const dv = evt.target.value;
          this.notifyLatest.set(cuuid, dv);
          this.notifyFlag.set(cuuid, true);
          this.notifyLatestUuid = cuuid;
        };
        characteristic._bleExtHandler = handler;
        characteristic.addEventListener('characteristicvaluechanged', handler);
      } catch (e) {
        this.errorMessage = String(e && e.message ? e.message : e);
      }
    }

    hasCharacteristic(args) {
      return this.characteristics.has(Scratch.Cast.toString(args.CUUID));
    }

    // ---------------- 読み書き ----------------
    async readCharacteristic(args) {
      const cuuid = Scratch.Cast.toString(args.CUUID);
      const c = this.characteristics.get(cuuid);
      if (!c) { this.errorMessage = 'キャラクタリスティック未取得: ' + cuuid; return; }
      try {
        const dv = await c.readValue();
        this.lastReadValue = dv;
      } catch (e) {
        this.errorMessage = String(e && e.message ? e.message : e);
      }
    }

    lastReadValueAs(args) {
      const format = VALUE_FORMATS[args.FORMAT] || 'text';
      return decodeFromDataView(this.lastReadValue, format);
    }

    async writeCharacteristic(args) {
      const cuuid = Scratch.Cast.toString(args.CUUID);
      const c = this.characteristics.get(cuuid);
      if (!c) { this.errorMessage = 'キャラクタリスティック未取得: ' + cuuid; return; }
      const format = WRITE_FORMATS[args.FORMAT] || 'text';
      const response = RESPONSE_TYPES[args.RESPONSE] || 'with';
      const bytes = encodeToBytes(args.VALUE, format);
      try {
        if (response === 'without' && c.writeValueWithoutResponse) {
          await c.writeValueWithoutResponse(bytes);
        } else if (c.writeValueWithResponse) {
          await c.writeValueWithResponse(bytes);
        } else {
          await c.writeValue(bytes);
        }
      } catch (e) {
        this.errorMessage = String(e && e.message ? e.message : e);
      }
    }

    // ---------------- 通知 ----------------
    async startNotifications(args) {
      const cuuid = Scratch.Cast.toString(args.CUUID);
      const c = this.characteristics.get(cuuid);
      if (!c) { this.errorMessage = 'キャラクタリスティック未取得: ' + cuuid; return; }
      try {
        await c.startNotifications();
      } catch (e) {
        this.errorMessage = String(e && e.message ? e.message : e);
      }
    }

    async stopNotifications(args) {
      const cuuid = Scratch.Cast.toString(args.CUUID);
      const c = this.characteristics.get(cuuid);
      if (!c) { this.errorMessage = 'キャラクタリスティック未取得: ' + cuuid; return; }
      try {
        await c.stopNotifications();
      } catch (e) {
        this.errorMessage = String(e && e.message ? e.message : e);
      }
    }

    whenNotified(args) {
      const cuuid = Scratch.Cast.toString(args.CUUID);
      if (this.notifyFlag.get(cuuid)) {
        this.notifyFlag.set(cuuid, false);
        return true;
      }
      return false;
    }

    lastNotifiedUuid() {
      return this.notifyLatestUuid;
    }

    notifiedValueAs(args) {
      const cuuid = Scratch.Cast.toString(args.CUUID);
      const format = VALUE_FORMATS[args.FORMAT] || 'text';
      return decodeFromDataView(this.notifyLatest.get(cuuid), format);
    }

    // ---------------- ディスクリプタ ----------------
    async getDescriptor(args) {
      const cuuid = Scratch.Cast.toString(args.CUUID);
      const duuid = Scratch.Cast.toString(args.DUUID);
      const c = this.characteristics.get(cuuid);
      if (!c) { this.errorMessage = 'キャラクタリスティック未取得: ' + cuuid; return; }
      try {
        const d = await c.getDescriptor(duuid);
        this.descriptors.set(cuuid + '|' + duuid, d);
      } catch (e) {
        this.errorMessage = String(e && e.message ? e.message : e);
      }
    }

    async readDescriptor(args) {
      const cuuid = Scratch.Cast.toString(args.CUUID);
      const duuid = Scratch.Cast.toString(args.DUUID);
      const d = this.descriptors.get(cuuid + '|' + duuid);
      if (!d) { this.errorMessage = 'ディスクリプタ未取得'; return; }
      try {
        this.lastDescriptorValue = await d.readValue();
      } catch (e) {
        this.errorMessage = String(e && e.message ? e.message : e);
      }
    }

    lastDescriptorValueAs(args) {
      const format = VALUE_FORMATS[args.FORMAT] || 'text';
      return decodeFromDataView(this.lastDescriptorValue, format);
    }

    async writeDescriptor(args) {
      const cuuid = Scratch.Cast.toString(args.CUUID);
      const duuid = Scratch.Cast.toString(args.DUUID);
      const d = this.descriptors.get(cuuid + '|' + duuid);
      if (!d) { this.errorMessage = 'ディスクリプタ未取得'; return; }
      const format = WRITE_FORMATS[args.FORMAT] || 'text';
      const bytes = encodeToBytes(args.VALUE, format);
      try {
        await d.writeValue(bytes);
      } catch (e) {
        this.errorMessage = String(e && e.message ? e.message : e);
      }
    }

    // ---------------- スキャン／アドバタイズ (実験的機能) ----------------
    async startScan() {
      if (!navigator.bluetooth.requestLEScan) {
        this.errorMessage = 'requestLEScanに未対応です。chrome://flags で実験的Web機能を有効にしてください';
        return;
      }
      try {
        this._scanHandler = (evt) => {
          this.newAdvertisementFlag = true;
          this.lastAdvertisement = {
            name: evt.device && evt.device.name,
            id: evt.device && evt.device.id,
            rssi: evt.rssi
          };
        };
        navigator.bluetooth.addEventListener('advertisementreceived', this._scanHandler);
        this._scan = await navigator.bluetooth.requestLEScan({ acceptAllAdvertisements: true, keepRepeatedDevices: true });
        this.scanning = true;
      } catch (e) {
        this.errorMessage = String(e && e.message ? e.message : e);
      }
    }

    stopScan() {
      if (this._scan) {
        this._scan.stop();
        this._scan = null;
      }
      if (this._scanHandler) {
        navigator.bluetooth.removeEventListener('advertisementreceived', this._scanHandler);
        this._scanHandler = null;
      }
      this.scanning = false;
    }

    isScanning() {
      return this.scanning;
    }

    whenAdvertisementReceived() {
      if (this.newAdvertisementFlag) {
        this.newAdvertisementFlag = false;
        return true;
      }
      return false;
    }

    lastAdvName() {
      return this.lastAdvertisement ? (this.lastAdvertisement.name || '') : '';
    }

    lastAdvRssi() {
      return this.lastAdvertisement ? (this.lastAdvertisement.rssi != null ? this.lastAdvertisement.rssi : '') : '';
    }

    lastAdvId() {
      return this.lastAdvertisement ? (this.lastAdvertisement.id || '') : '';
    }

    async watchAdvertisements() {
      if (!this.device || !this.device.watchAdvertisements) {
        this.errorMessage = 'watchAdvertisementsに未対応、またはデバイス未接続です';
        return;
      }
      try {
        await this.device.watchAdvertisements();
      } catch (e) {
        this.errorMessage = String(e && e.message ? e.message : e);
      }
    }

    // ---------------- UUIDユーティリティ ----------------
    commonServiceUuid(args) {
      return SERVICE_UUIDS[args.SVC] || '';
    }

    commonCharacteristicUuid(args) {
      return CHARACTERISTIC_UUIDS[args.CHR] || '';
    }

    canonicalUuid(args) {
      const v = Scratch.Cast.toString(args.UUID);
      try {
        if (typeof BluetoothUUID !== 'undefined' && BluetoothUUID.canonicalUUID) {
          const n = Number(v);
          if (!isNaN(n) && v.trim() !== '') {
            return BluetoothUUID.canonicalUUID(n);
          }
        }
      } catch (e) {
        // fallthrough
      }
      return v;
    }
  }

  Scratch.extensions.register(new BLEExtension(Scratch.vm ? Scratch.vm.runtime : null));
})(Scratch);
