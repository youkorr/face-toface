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
- **Signalisation d'appel** : **native, intégrée à face2face** (aucune dépendance
  externe). Messages de contrôle UDP `CALL/RING/ANSWER/HANGUP/DECLINE` + machine
  à états `IDLE → OUTGOING/RINGING → STREAMING`.

### Signalisation d'appel (absorbée de l'intercom, sans en dépendre)

L'ancienne version reposait sur l'external `esphome-intercom`. Ce qui était bon
(audio PCM 16k/16-bit mono, FSM d'appel, états) a été **réimplémenté nativement**
dans `face2face`, puis la dépendance a été **supprimée**.

**Actions** (pour boutons HA / clics LVGL) :

```yaml
on_press:
  - face2face.call: f2f      # appeler le pair
  - face2face.answer: f2f    # décrocher
  - face2face.hangup: f2f    # raccrocher / annuler
  - face2face.decline: f2f   # refuser
```

**Triggers** (déclarés dans le bloc `face2face:`) :
`on_ringing`, `on_outgoing_call`, `on_streaming`, `on_idle`
(+ option `auto_answer: true` pour un mode interphone, `ring_timeout`).

### Annulation d'écho (AEC) — ESP-SR, intégrée

Le mains-libres est géré par l'**AEC d'Espressif ESP-SR** (`esp_aec`), intégrée
directement dans face2face. À chaque trame micro : `aec_process(mic, référence)`
où la **référence** = l'audio reçu du pair (ce que joue le HP). Le résultat
nettoyé est envoyé au pair.

```yaml
face2face:
  enable_aec: true            # défaut ; pulle esp-sr et compile le chemin AEC
  aec_mode: voip_high_perf    # sr_low_cost | voip_low_cost | voip_high_perf | fd_*
  aec_filter_length: 4        # 1-8 (plus grand = plus de RAM/CPU)
```

- `enable_aec: false` → aucun appel esp-sr, aucune dépendance ajoutée.
- Trames alignées via un ring buffer de référence ; si le HP est silencieux, la
  référence est nulle (pas d'écho à annuler).
- Buffers `int16` alignés 16 o (`heap_caps_aligned_alloc`), 16 kHz mono, comme
  recommandé par Espressif.

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
  standalone-facetime.yaml      # CONFIG COMPLÈTE prête à flasher (carte vierge)
  face2face-snippet.yaml        # bloc face2face (vidéo + audio + appel) à coller
  lvgl-call-page.yaml           # page d'appel LVGL 9.5 moderne (1024x600) + présence
```

> ✅ Le composant face2face (vidéo + audio + appel), la page d'appel et la présence
> sont déjà **fusionnés** dans votre `waveshare (3).yaml` (page LVGL `call_page`),
> **sans aucune dépendance intercom**. `example/standalone-facetime.yaml` est une
> config minimale séparée et complète.

## 4bis. Présence : « l'autre est-il connecté ? »

`face2face` envoie un **heartbeat UDP** (1/s) au pair, en permanence (même hors
appel). Chaque carte sait donc si l'autre est joignable :

```cpp
id(f2f).peer_online()        // true si paquet reçu du pair < 4 s
id(f2f).peer_last_seen_ms()  // millis() du dernier paquet reçu
```

Exposé en YAML via un `binary_sensor` template (→ Home Assistant) et une pastille
verte/grise dans la barre de statut LVGL (voir `example/lvgl-call-page.yaml`).

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

6. **AEC / esp-sr** : `enable_aec: true` (défaut) ajoute le composant managé
   `espressif/esp-sr` (gros). À la 1ʳᵉ compilation, vérifiez la `ref` esp-sr dans
   `__init__.py` (par défaut `master`) et l'espace flash. Si vous partagez le micro
   avec `voice_assistant`/`micro_wake_word`, coupez-les pendant l'appel (le micro
   ne peut servir deux pipelines de capture simultanés proprement).

## 7. Pistes d'évolution

- **H.264** au lieu de MJPEG (votre `CONFIG_ESP_H264_DUAL_TASK` est déjà activé) :
  meilleur débit, mais gestion des I-frames sur UDP à coder.
- **Jitter buffer audio** (petit tampon de ré-ordonnancement) pour lisser le réseau.
- **Délai de référence AEC** ajustable si l'écho persiste (aligner ref/mic).
- **Découverte** via Home Assistant au lieu d'IP codée en dur.

## Sources

- Codec JPEG matériel P4 (`esp_driver_jpeg`) : ESP-IDF `components/esp_driver_jpeg`
- AEC ESP-SR : <https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/audio_front_end/README.html>
- Protocole repris (puis réimplémenté) de : <https://github.com/n-IA-hane/esphome-intercom>
- Vos composants : <https://github.com/youkorr/test2_esp_video_esphome>, <https://github.com/youkorr/lvgl_9.5>
