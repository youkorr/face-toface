# face-toface

Communication vidéo + audio en **peer-to-peer (IP directe)**, façon FaceTime,
entre **deux ESP32-P4**, sous **ESPHome / Home Assistant**, avec caméra
**OV5647 (MIPI-CSI)**, affichage **MIPI-DSI + LVGL 9.5**, et codec **MJPEG
matériel**.

> ⚠️ **État : squelette fonctionnel + points d'intégration matériels à câbler.**
> La couche réseau (sockets UDP, fragmentation/réassemblage JPEG, glue ESPHome,
> rendu LVGL) est écrite et complète. Les appels aux codecs/capture **matériels**
> d'Espressif (`esp_capture`, JPEG hardware, `esp_codec_dev`) sont balisés comme
> « Integration point » dans `face2face.cpp` et doivent être finalisés/testés
> **sur la carte** (impossible à compiler/tester hors matériel ESP32-P4).

---

## 1. Pourquoi pas « tout en YAML » ?

ESPHome n'a **aucun** composant natif de streaming/appel vidéo. La solution est
donc un **composant externe C++** (`components/face2face/`) que l'on **configure
ensuite en YAML** — exactement le modèle utilisé par `esp_video` lui-même.
Vous gardez donc une config 100 % YAML côté utilisateur.

## 2. Le matériel s'y prête (vérifié)

| Brique | ESP32-P4 | Composant utilisé |
|---|---|---|
| Caméra OV5647 MIPI-CSI | ISP + V4L2 | `esp_video` (déjà OK chez vous) |
| Encodage MJPEG | **JPEG matériel** (720p@88fps / 1080p@34fps) | `esp_capture` |
| Décodage MJPEG | **JPEG matériel** (720p@88fps) | `esp_video_codec` |
| Audio I2S (mic + HP) | I2S + codec | `esp_codec_dev` |
| Affichage | MIPI-DSI | `lvgl` (ESPHome) |

> 💡 Le P4 a **aussi** un encodeur **H.264 matériel** *et* un décodeur H.264
> (`esp_h264`). On commence en **MJPEG** car encode **et** décode sont matériels,
> sans état entre images → très tolérant aux pertes UDP. Passage à H.264 possible
> plus tard (meilleure bande passante, gestion des I-frames à coder).

## 3. Architecture

```
   CARTE A                          réseau                      CARTE B
 ┌─────────────────────────┐                          ┌─────────────────────────┐
 │ OV5647 ──► esp_video     │                          │ OV5647 ──► esp_video     │
 │   (V4L2 MIPI-CSI)        │                          │   (V4L2 MIPI-CSI)        │
 │        │                 │                          │        │                 │
 │   esp_capture            │   UDP :9000 (vidéo)      │   esp_capture            │
 │   ├─ MJPEG enc (HW) ─────┼───────────────────────►  │   ├─ JPEG dec (HW) ──►   │
 │   └─ audio enc ──────────┼───────────────────────►  │   └─ audio dec ──► I2S HP │
 │                          │   UDP :9001 (audio)      │                          │
 │   JPEG dec (HW) ◄────────┼───────────────────────   │   MJPEG enc (HW) ◄─────── │
 │        │                 │                          │                          │
 │   LVGL canvas (RGB565)   │                          │   LVGL canvas (RGB565)   │
 └─────────────────────────┘                          └─────────────────────────┘
```

### Protocole sur le fil (UDP)

Chaque image JPEG / bloc audio est **fragmenté** en paquets ≤ 1400 octets
(pour éviter la fragmentation IP). En-tête `F2FHeader` (voir `face2face.h`) :

```
magic(4) stream(1) flags(1) frame_id(2) frag_index(2) frag_count(2)
frame_size(4) payload_len(2)  | payload...
```

Le récepteur réassemble par `(stream, frame_id)`, décode l'image complète, et
abandonne les images partielles d'un `frame_id` plus ancien → **latence faible**,
robuste aux pertes (on saute simplement l'image perdue, pas de blocage).

## 4. Fichiers

```
components/face2face/
  __init__.py      # schéma YAML + pull des managed components Espressif
  face2face.h      # protocole UDP + classe Component
  face2face.cpp    # réseau/réassemblage (complet) + codecs (à câbler)
example/
  face2face-device.yaml   # config des deux cartes (changez peer_ip)
```

## 5. Configuration YAML

```yaml
external_components:
  - source: { type: local, path: ../components }

face2face:
  id: f2f
  peer_ip: "192.168.1.51"   # IP de l'autre carte
  video_port: 9000
  audio_port: 9001
  width: 640
  height: 480
  framerate: 15
  jpeg_quality: 80
  enable_audio: true
  audio_sample_rate: 16000
```

Contrôle de l'appel (depuis un bouton HA ou un widget LVGL) :

```yaml
button:
  - platform: template
    name: "Appeler"
    on_press: { lambda: "id(f2f).start_call();" }
  - platform: template
    name: "Raccrocher"
    on_press: { lambda: "id(f2f).stop_call();" }
```

Affichage de l'image distante dans un `canvas` LVGL : voir le bloc `interval:`
dans `example/face2face-device.yaml`.

## 6. Réseau : l'ESP32-P4 n'a pas de WiFi

Le P4 est **sans radio**. Pour l'IP il faut :
- l'**Ethernet** de la carte d'éval (utilisé dans l'exemple), ou
- un co-processeur **ESP32-C6/C5** en **ESP-Hosted** (`wifi:` via SDIO).

Les deux cartes doivent être sur le **même réseau** (IP directe P2P).

## 7. Ce qu'il reste à finaliser (sur carte)

Dans `face2face.cpp`, remplacez les corps marqués `// === Integration point ===` :

1. **`capture_init_` / `pump_tx_`** — pipeline `esp_capture` : source V4L2
   (OV5647) + source audio I2S, sink MJPEG + audio, `acquire_frame`/`release_frame`.
2. **`decoder_init_` / `decode_jpeg_`** — décodeur JPEG matériel → RGB565.
3. **`audio_init_` / `audio_play_`** — `esp_codec_dev` pour la sortie HP.
4. Ajustez les **refs de versions** des managed components dans `__init__.py`
   à ce qui existe au moment du build (registry Espressif).

## 8. Alternative « production » : ESP-WebRTC

Espressif fournit `esp-webrtc-solution` (composant `esp_peer`) avec une démo
**peer-to-peer entre deux ESP32-P4** (audio + vidéo, signalisation incluse).
C'est plus lourd (pile WebRTC, VP8/H.264/Opus) mais c'est la voie si vous voulez
traverser des réseaux/NAT plus tard. Le présent projet privilégie la simplicité
(MJPEG + UDP direct) pour démarrer vite sur LAN.

## Sources

- ESP32-P4 codecs JPEG/H.264 : <https://components.espressif.com/components/espressif/esp_h264>, <https://developer.espressif.com/blog/2025/07/esp-h264-use-tips/>
- ESP-GMF / esp_capture : <https://github.com/espressif/esp-gmf>
- esp_video_codec : <https://components.espressif.com/components/espressif/esp_video_codec>
- esp-webrtc-solution (peer P2P P4) : <https://github.com/espressif/esp-webrtc-solution>
- OV5647 MIPI-CSI sur P4 (UVC/MJPEG) : <https://github.com/r4d10n/esp32p4-uvc-video>
