# face-toface

Appel **vidéo + audio en peer-to-peer (IP directe, UDP)**, façon FaceTime, entre
**deux ESP32-P4** sous **ESPHome / Home Assistant**.

Conçu pour s'intégrer à **votre** stack Waveshare existante :
- caméra **OV5647** via votre composant `esp_cam_sensor` (`tab5_cam`)
- micro **ES7210** (`esp32_microphone`) + HP **ES8311** (`esp32_speaker`)
- affichage **MIPI-DSI + LVGL 9.5** (canvas)
- codec **JPEG matériel** natif du P4 (`esp_driver_jpeg`)

> ⚠️ **État : composant complet, à valider sur carte.** La logique réseau
> (UDP, fragmentation/réassemblage), l'intégration caméra (`get_current_rgb_frame`),
> micro/HP (callbacks ESPHome) et le pipeline JPEG matériel sont écrits avec les
> vraies API. Je **ne peux pas compiler du firmware P4 ici** : prévoyez 1-2
> itérations sur le matériel, notamment pour les noms d'enums `esp_driver_jpeg`
> qui varient légèrement selon la version d'ESP-IDF (voir §6).

---

## 1. Le flux (chaque carte fait les deux sens)

```
  ÉMISSION (TX)                                   RÉCEPTION (RX)
  ┌──────────────────────────┐                    ┌──────────────────────────┐
  │ tab5_cam (OV5647)         │                    │  UDP :9000 (vidéo)        │
  │  get_current_rgb_frame()  │                    │   réassemblage JPEG       │
  │        │ RGB565           │                    │        │                  │
  │  JPEG enc MATÉRIEL  ──────┼── UDP :9000 ──►     │  JPEG dec MATÉRIEL        │
  │        │ JPEG             │                     │        │ RGB565           │
  │  sendto(peer)             │                     │  remote_rgb565()          │
  │                           │                     │        │ (lambda YAML)    │
  │ esp32_microphone          │                     │  lv_canvas_set_buffer     │
  │  callback PCM 16k ────────┼── UDP :9001 ──►      │  ──► LVGL canvas          │
  │                           │   (audio)            │                          │
  │ speaker.play() ◄──────────┼── UDP :9001 ───      │  micro du pair            │
  └──────────────────────────┘                     └──────────────────────────┘
```

- **Vidéo** : RGB565 (caméra) → **JPEG matériel** → UDP. À la réception :
  réassemblage → **JPEG matériel** → RGB565 → canvas LVGL. MJPEG = sans état
  entre images, donc une image perdue est simplement sautée (latence faible).
- **Audio** : PCM 16 bit / 16 kHz mono brut sur UDP (~32 ko/s). Le micro ESPHome
  pousse les blocs via callback ; on les rejoue sur le speaker du pair.

## 2. Pourquoi un composant custom (et pas `camera_web_server`)

`camera_web_server` (MJPEG over HTTP/TCP) ne tient pas bien la charge en temps
réel. `face2face` envoie le JPEG en **UDP** directement au pair : pas de serveur,
pas de TCP, latence minimale, et on saute les images perdues au lieu de bloquer.

## 3. Protocole sur le fil (UDP)

Chaque image JPEG / bloc audio est **fragmenté** en paquets ≤ 1400 o. En-tête
`F2FHeader` (`face2face.h`) : `magic, stream, flags, frame_id, frag_index,
frag_count, frame_size, payload_len`. Le récepteur réassemble par
`(stream, frame_id)` et abandonne les fragments d'un `frame_id` plus ancien.

## 4. Fichiers

```
components/face2face/
  __init__.py      # schéma YAML, refs caméra/micro/HP
  face2face.h      # protocole UDP + classe Component
  face2face.cpp    # UDP + JPEG matériel + caméra + audio
example/
  face2face-snippet.yaml   # à coller dans votre waveshare.yaml
```

## 5. Intégration YAML

Voir `example/face2face-snippet.yaml`. L'essentiel :

```yaml
face2face:
  id: f2f
  peer_ip: "192.168.1.51"     # IP de l'autre carte
  camera_id: tab5_cam
  microphone_id: esp32_microphone
  speaker_id: media_resampling_speaker   # resampler 16k -> 48k
  width: 640
  height: 480
  framerate: 15
  jpeg_quality: 40
```

Affichage : un widget `canvas` (`id: remote_video`) + un `interval` qui appelle
`lv_canvas_set_buffer(...)` avec `id(f2f).remote_rgb565()`. Self-view local : votre
`lvgl_camera_display` existant sur un petit canvas.

## 6. À valider / ajuster sur carte

1. **`width`/`height`** doivent correspondre à la sortie RGB de `tab5_cam`
   (vous êtes en `800x800` ; mettez la même chose ou ajoutez un redimensionnement).
2. **Enums `esp_driver_jpeg`** : selon votre ESP-IDF, vérifiez les noms exacts
   dans `driver/jpeg_encode.h` / `driver/jpeg_decode.h` :
   `JPEG_ENCODE_IN_FORMAT_RGB565`, `JPEG_DOWN_SAMPLING_YUV420`,
   `JPEG_DECODE_OUT_FORMAT_RGB565`, `JPEG_DEC_RGB_ELEMENT_ORDER_RGB`,
   `JPEG_ENC_ALLOC_INPUT_BUFFER`, etc.
3. **Speaker** : `media_resampling_speaker` (sortie 48 kHz) accepte le PCM 16 kHz
   via `set_audio_stream_info(16, 1, 16000)`. Si l'audio est trop rapide/lent,
   c'est ce mapping qu'il faut ajuster.
4. **Débit** : à 640x480@15fps q=40, comptez ~3-6 Mbps. Si saccades, baissez
   `framerate`, la résolution ou `jpeg_quality`.
5. **WiFi via ESP-Hosted (C6)** : les deux cartes sur le même LAN.

## 7. Pistes d'évolution

- **H.264** au lieu de MJPEG (votre `CONFIG_ESP_H264_DUAL_TASK` est déjà activé) :
  meilleur débit, mais gestion des I-frames sur UDP à coder.
- **Logique d'appel** (sonnerie, décrocher/raccrocher, état occupé) côté LVGL.
- **Découverte** via Home Assistant au lieu d'IP codée en dur.
- Alternative « production » longue distance : `esp-webrtc-solution` (composant
  `esp_peer`, démo P2P deux ESP32-P4) — pile WebRTC complète, traverse les NAT.

## Sources

- Codec JPEG matériel P4 (`esp_driver_jpeg`) : ESP-IDF `components/esp_driver_jpeg`
- esp-webrtc-solution (P2P P4) : <https://github.com/espressif/esp-webrtc-solution>
- Vos composants : <https://github.com/youkorr/test2_esp_video_esphome>, <https://github.com/youkorr/lvgl_9.5>
