# Handoff — Intégration esphome-webrtc + face-toface (+ lvgl_9.5)

> Document de passation pour une nouvelle session Claude Code.
> Contexte : youkorr va intégrer `esphome-webrtc`, `face-toface` et
> éventuellement `lvgl_9.5` dans un même écosystème ESP32-P4 / ESPHome 2026.7+.
> Rédigé le 2026-07-13 en fin de session (session précédente : corrections
> licences + crash audio I2S/GDMA, voir §2).

---

## 1. État des dépôts et branches

| Dépôt | Branche à utiliser | Contenu |
|---|---|---|
| `youkorr/face-toface` | `claude/mit-license-face-to-face-p9fc6m` | = `main` + 3 fixes fdaudio (voir §2). **Validé sur matériel** (Waveshare + Habbit P4) : plus aucune erreur audio. À merger dans `main`. |
| `youkorr/lvgl_9.5` | `claude/mit-license-face-to-face-p9fc6m` | = `claude/jolly-lamport-ERZOC` (branche de dev ACTIVE de youkorr, 10+ commits devant main : mipi_dsi zero-copy, shims ESPHome 2026.8, lottie_state_machine) + commit `8509b22` (réserve SRAM interne, voir §2). ⚠️ Ne PAS repartir de `main` : main est en retard. |
| `youkorr/esphome-webrtc` | `claude/repository-file-tracking-r00ybc` | Composant `webrtc` neuf (port esp-webrtc-solution 0.9.1). Revue de code faite : **2 bugs à corriger avant intégration** (voir §3). |
| `youkorr/esphome_esp-video` | `claude/usb-uvc-consumer-integration-gnbflf` (utilisée par youkorr) | Caméra MIPI-CSI/UVC (esp_video, esp_cam_sensor, lvgl_camera_display). Non touchée dans la dernière session (licences corrigées sur main plus tôt). |

Le YAML de production de youkorr référence ces dépôts via `external_components`
(refresh: always). Les cartes cibles : ESP32-P4 (Waveshare 1024×600, Habbit,
M5Stack Tab5), Wi-Fi via ESP-Hosted C6, PSRAM hex 200 MHz obligatoire.

---

## 2. Acquis de la session précédente — NE PAS RÉGRESSER

Trois causes racines identifiées et corrigées pour le crash audio au boot
(`micro_wake_word` → `fdaudio` → I2S) sous ESPHome 2026.7.0-dev :

1. **`fdaudio` fuyait ses canaux I2S** en cas d'échec d'init → tous les retries
   échouaient définitivement (`i2s_new_channel: no available channel found`).
   Fix : `face-toface@02ba150` (release/disable/delete sur tous les chemins
   d'échec de `init_i2s_()` + rollback du refcount `consumers_` dans
   `engine_start`).

2. **Le fork LVGL vidait la SRAM interne DMA** : `alloc_draw_buf` (jusqu'à
   3 buffers pleine taille : draw + rotate + double-buffer) prenait la SRAM
   interne sans limite, et le pipeline `rotation_buffers_internal` ne gardait
   qu'une marge relative de 30 %. Fix : `lvgl_9.5@8509b22` — constante
   `INTERNAL_DMA_RESERVE = 160 * 1024` (plancher ABSOLU) appliquée au budget
   rotation ET au chemin interne-d'abord du lambda `alloc_draw_buf` dans
   `components/lvgl/lvgl_esphome.cpp`. Logs ajoutés
   (`draw buffer X KB redirected to PSRAM...`).

3. **Politique mémoire ESPHome 2026.7 (« IDF pur ») + GDMA IRAM-safe** : le
   driver I2S alloue sa structure de canal en `MALLOC_CAP_DEFAULT`, qui peut
   désormais partir en PSRAM ; avec `CONFIG_GDMA_ISR_IRAM_SAFE=y` (activé par
   la pile vidéo/LCD), GDMA refuse le contexte →
   `gdma: user context not in internal RAM` →
   `i2s_channel_init_std_mode failed`, quelle que soit la RAM libre.
   Fix : `face-toface@f7ea546` — `fdaudio/__init__.py` ajoute
   `esp32.add_idf_sdkconfig_option("CONFIG_I2S_ISR_IRAM_SAFE", True)`.
   **C'EST LE FIX DÉCISIF** (validé matériel). Toute future intégration qui
   fait de l'I2S sur ces firmwares doit garder cet appairage.

Rappel breaking-change 2026.2+ : ESPHome exclut les composants IDF intégrés
non utilisés ; un composant externe qui en a besoin doit appeler
`include_builtin_idf_component()` dans `to_code()` (ou l'utilisateur passe par
`esp32: framework: advanced: include_builtin_idf_components:`). Symptôme :
`fatal error: xxx.h: No such file or directory`. Les composants du registre
Espressif (managed) tirent leurs dépendances tout seuls — le problème ne
concerne que les REQUIRES vendorisés.

Licences : déjà en règle sur les 4 dépôts (MIT youkorr + tiers préservés :
Apache-2.0 Espressif vendorisé dans esphome_esp-video, dual MIT/GPLv3 dans
lvgl_9.5, Espressif-Modified-MIT reconnu dans esphome-webrtc). Copyright :
`youkorr` uniquement (pas de nom réel).

---

## 3. esphome-webrtc — revue de code (à corriger AVANT intégration)

Qualité globale : très bonne (meilleure que `webrtc_call` de face-toface).
Bugs et durcissements identifiés, par ordre de priorité :

1. **BUG — file d'événements de profondeur 1** (`webrtc.h`,
   `volatile int pending_event_`) : les événements du task esp_webrtc
   (PAIRED puis CONNECTED à quelques ms d'écart) s'écrasent avant que
   `loop()` ne les draine → triggers perdus, état incohérent. Remplacer par
   un bitmask `std::atomic<uint32_t>` d'événements en attente (ou petit ring
   lock-free), drainé dans `loop()`. `volatile` n'est pas une primitive de
   synchro.

2. **BUG — pointeurs pendants dans `open_()`** (`webrtc.cpp`) : `url`
   (std::string locale) et `servers` (std::vector local) meurent en fin de
   fonction alors que `cfg.signaling_cfg.signal_url` et
   `cfg.peer_cfg.server_lists` pointent dedans, et que esp_webrtc peut garder
   ces pointeurs. Les promouvoir en membres de `WebRTCComponent`.

3. **Ajouter `CONFIG_I2S_ISR_IRAM_SAFE=True`** dans `__init__.py` (même fix
   décisif que §2.3 — ce composant fait de l'I2S via codec_board sur des
   firmwares GDMA-IRAM-safe).

4. **`auto_start` trop tôt** : lancé dans `setup()` (priorité LATE) alors que
   le Wi-Fi ESP-Hosted n'est pas associé → la signalisation échoue au boot.
   Déférer dans `loop()` jusqu'à `network::is_connected()`.

5. **Résolution vidéo codée en dur** (`1024×600 @10fps` dans `open_()`) :
   exposer `video_width` / `video_height` / `fps` en YAML.

6. Mineurs : utiliser directement les enums `ESP_WEBRTC_EVENT_*` dans le
   switch (esp_webrtc.h est déjà inclus dans le .cpp) au lieu des constantes
   `EV_* = 1..5` recopiées ; vérifier les retours de `init_codec()` /
   `esp_capture_new_*` dans `media_sys.c` ; appeler
   `webrtc_media_sys_teardown()` à l'arrêt ; ajouter l'en-tête
   `SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT` à
   `media_sys.c`.

---

## 4. Plan / contraintes d'intégration

**Architecture cible : DEUX firmwares, pas un.**

- **Firmware « LAN »** : lvgl_9.5 + esphome_esp-video (esp_video,
  esp_cam_sensor, lvgl_camera_display) + face-toface (face2face, fdaudio) +
  micro_wake_word/voice_assistant. C'est la config actuelle de youkorr —
  fonctionne après les fixes §2.
- **Firmware « WebRTC »** : esphome-webrtc SEUL (+ réseau). Il POSSÈDE la
  caméra, l'I2S et le LCD via codec_board/GMF (esp_capture/av_render) →
  **incompatible dans le même YAML** avec esp_video vendorisé, fdaudio et
  l'affichage LVGL. En particulier : `espressif/esp_video 1.0.0` (registre,
  tiré par esphome-webrtc) ≠ `esp_video` vendorisé d'esphome_esp-video —
  collision de composants si mélangés.

**Actions d'intégration recommandées :**

1. Corriger les points §3 sur `esphome-webrtc` (branche
   `claude/repository-file-tracking-r00ybc` ou nouvelle branche).
2. Déclarer `esphome-webrtc` successeur officiel de `webrtc_call` :
   marquer `components/webrtc_call` de face-toface déprécié dans le README
   (section §6 du README actuel) avec lien vers le nouveau dépôt.
3. lvgl_9.5 : replier le commit `8509b22` (INTERNAL_DMA_RESERVE) dans la
   branche de dev `claude/jolly-lamport-ERZOC` (ou faire de la branche
   handoff la nouvelle branche de dev). Ne pas perdre la réserve lors des
   merges futurs — c'est elle qui protège l'audio.
4. Merger `claude/mit-license-face-to-face-p9fc6m` de face-toface dans `main`
   (fixes validés matériel).
5. Ajouter au README d'esphome-webrtc l'avertissement « firmware dédié »
   (comme le fait déjà le README de face-toface pour webrtc_call) + un
   exemple YAML complet.
6. Vérifier `sdkconfig` communs si des composants partagent un firmware :
   `CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE=False` est posé par fdaudio ET
   esphome-webrtc (cohérent), `CONFIG_I2S_ISR_IRAM_SAFE=True` doit l'être
   partout où il y a de l'I2S.

**Pièges connus sur cette base matérielle :**

- Un seul moteur JPEG hardware sur le P4 (encode+decode sérialisés).
- ES7210 : un seul consommateur de capture à la fois — suspendre
  voice_assistant/micro_wake_word pendant un appel (`audio_start_delay`).
- `rotation_buffers_internal: true` (lvgl_9.5) consomme la SRAM interne par
  design — la réserve de 160 Ko doit rester le garde-fou.
- Sur échec I2S : les messages `i2s_channel_disable: the channel has not
  been enabled yet` pendant le cleanup fdaudio sont bénins (disable défensif).
