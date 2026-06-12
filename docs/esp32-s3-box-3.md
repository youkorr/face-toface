# ESP32-S3-BOX-3 - Audio full-duplex avec fdaudio

Ce document explique comment configurer et faire fonctionner l'audio
full-duplex (microphone et haut-parleur simultanes) sur l'ESP32-S3-BOX-3 a
l'aide du composant `fdaudio`, ainsi que l'annulation d'echo (AEC / AFE) basee
sur esp-sr.

Le fichier d'exemple complet est `example/fdaudio-s3box3-duplex-test.yaml`.

## 1. Materiel

L'ESP32-S3-BOX-3 est une carte ESP32-S3 N16R8 (16 Mo de flash, 8 Mo de PSRAM
octale). Son sous-systeme audio comporte deux codecs distincts sur le meme bus
I2S et le meme bus I2C :

| Role                | Codec   | Adresse I2C | Sens   |
| ------------------- | ------- | ----------- | ------ |
| Haut-parleur (sortie) | ES8311  | 0x18        | DAC    |
| Microphones (entree)  | ES7210  | 0x40        | ADC    |

L'amplificateur du haut-parleur (PA) est commande par GPIO46. Il est coupe au
reset : il doit etre mis a l'etat haut au demarrage, sinon le codec joue mais
aucun son ne sort.

### Brochage

| Signal       | GPIO    |
| ------------ | ------- |
| I2C SDA      | GPIO8   |
| I2C SCL      | GPIO18  |
| I2S MCLK     | GPIO2   |
| I2S BCLK     | GPIO17  |
| I2S WS/LRCLK | GPIO45  |
| I2S DIN (mic)| GPIO16  |
| I2S DOUT (HP)| GPIO15  |
| PA_CTRL (ampli) | GPIO46 |

## 2. Principe du full-duplex

`fdaudio` ouvre un seul port I2S en mode full-duplex : un appel a
`i2s_new_channel` cree simultanement un handle TX et un handle RX qui partagent
la meme horloge. La meme interface de donnees I2S est partagee par l'ES8311
(sortie) et l'ES7210 (entree) via `esp_codec_dev`. Le microphone et le
haut-parleur fonctionnent donc en meme temps sur la meme horloge, ce qui est la
condition d'un vrai full-duplex.

Le codec tourne a 48 kHz (horloge reellement stable pour l'ES7210). Cote
ESPHome :

- le microphone est decime de 48 kHz vers 16 kHz (moyenne par groupes de 3
  echantillons) avant d'etre expose au `voice_assistant` ou a `face2face` ;
- le haut-parleur recoit l'audio a 48 kHz ; les sources a 16 kHz sont
  sur-echantillonnees (interpolation lineaire) avant ecriture.

Le composant expose un `microphone` et un `speaker` ESPHome standards, donc
`voice_assistant`, `media_player` et `face2face` les utilisent sans
modification.

## 3. Configuration fdaudio

```yaml
fdaudio:
  id: audio_engine
  mclk_pin: GPIO2
  bclk_pin: GPIO17
  lrclk_pin: GPIO45
  din_pin: GPIO16
  dout_pin: GPIO15
  i2c_port: 0
  output_codec: es8311
  output_address: 0x18
  mic_address: 0x40
  mic_gain_db: 37.5
  output_volume: 70
  sample_rate: 16000          # cadence exposee a ESPHome
  codec_sample_rate: 48000    # horloge I2S/codec reelle, decimee vers 16 kHz
  mic_channels: 1             # ES7210 : MIC1=1, MIC2=2, MIC3=4, MIC4=8
  enable_aec: true
  use_afe: true
  aec_gate_ms: 250
```

### Reference des options principales

| Option              | Defaut | Role |
| ------------------- | ------ | ---- |
| `sample_rate`       | 16000  | Cadence du micro exposee a ESPHome. |
| `codec_sample_rate` | 48000  | Horloge I2S/codec reelle. Doit etre un multiple entier de `sample_rate`. |
| `mic_gain_db`       | 37.5   | Gain analogique de l'ES7210 (0 a 42 dB). |
| `mic_channels`      | 1      | Masque des entrees micro de l'ES7210. A changer si le micro est quasi muet. |
| `mic_digital_gain`  | 1.0    | Boost logiciel du micro decime (1.0 a 16.0). |
| `mic_agc`           | 0      | Controle automatique de gain vers un niveau cible (0 = off). |
| `noise_gate`        | 0      | Seuil de porte de bruit logicielle (0 = off). |
| `echo_suppression`  | 0      | Ducking far-end pour les appels, en pourcentage (0 = off). |
| `enable_aec`        | true   | Active l'AEC simple esp-sr (aec_create). |
| `use_afe`           | false  | Active l'AFE complete esp-sr (AEC + NS + AGC). |
| `aec_gate_ms`       | 250    | Fenetre d'adaptation de l'AEC apres activite du HP (0 = off). |

## 4. Annulation d'echo : AEC simple ou AFE complete

Deux chemins sont disponibles. Tous deux reposent sur esp-sr.

### AEC simple (`enable_aec: true`, `use_afe: false`)

Chemin leger base sur `aec_create` en mode `AEC_MODE_SR_LOW_COST`. Le traitement
tourne en ligne dans la tache microphone. Consommation modeste (quelques
kilo-octets de tampons, charge CPU moderee). Mode recommande pour la
coexistence avec `voice_assistant` et `micro_wake_word`, car l'AEC lineaire
preserve les caracteristiques spectrales utiles au mot d'eveil neuronal.

### AFE complete (`use_afe: true`)

Pipeline esp-sr complet : AEC, suppression de bruit (NS) et controle automatique
de gain (AGC), avec une reference far-end alignee. Format d'entree "MNR"
(microphone, canal nul, reference). Bien plus lourd :

- flash : esp-sr, esp-dl et esp-dsp ajoutent de l'ordre de 1 Mo au binaire ;
- PSRAM obligatoire (plusieurs centaines de kilo-octets de tampons internes) ;
- charge CPU significative et continue ;
- taches dediees (lecture du codec et alimentation de l'AFE, plus le thread de
  traitement interne d'esp-sr).

L'AFE ne se justifie que si l'AEC simple ne supprime pas suffisamment l'echo.

### Placement sur les coeurs

Les taches temps reel (lecture/ecriture du codec, microphone, haut-parleur)
sont sur le coeur 1. Le traitement lourd de l'AFE (thread interne esp-sr et
tache d'alimentation) est place sur le coeur 0, afin de ne pas affamer le
chemin audio temps reel. Sans cette separation, le son peut devenir hache.

## 5. Gating de l'AEC (aec_gate_ms)

Un echo n'existe dans le micro que pendant (ou juste apres) la lecture du
haut-parleur. En dehors de cette fenetre, il n'y a rien a annuler ; si le filtre
adaptatif continue de tourner sur un signal microphone seul, il derive et finit
par attenuer la vraie voix.

`aec_gate_ms` definit la fenetre d'adaptation apres une activite du
haut-parleur :

- pendant cette fenetre, l'AEC adapte normalement ;
- au-dela (silence reel du HP), l'adaptation est gelee :
  - en AEC simple, `aec_process` est saute et le micro passe sans traitement ;
  - en AFE, la reference est mise a zero (l'anneau est tout de meme vide pour
    conserver l'alignement micro/reference).

Reglage :

- 250 ms (defaut) couvrent la traine acoustique et codec habituelle ;
- augmenter (350 a 500) si le debut des phrases est coupe juste apres l'arret
  du HP ;
- diminuer (150 a 200) si trop d'echo passe immediatement apres l'arret du HP ;
- `aec_gate_ms: 0` desactive le gating (AEC toujours active, ancien
  comportement).

L'etat du gating est affiche dans les logs au demarrage :

```
AEC: enabled (gate: on)
AEC gate window: 250 ms
```

## 6. Dependances et alignement esp-dsp

Activer `enable_aec` ou `use_afe` tire automatiquement esp-sr dans le firmware.
esp-sr et esp-dl (ce dernier etant aussi tire par `micro_wake_word`) declarent
chacun leur propre dependance sur esp-dsp. Sans version commune imposee, le
gestionnaire de composants IDF peut resoudre des versions d'esp-dsp differentes
et incompatibles ; la pile audio/ML faute alors au demarrage et l'on perd a la
fois le haut-parleur et le microphone, alors que le meme firmware sans AEC
fonctionne (car esp-sr n'est jamais tire).

Pour eviter cela, le composant `fdaudio` epingle lui-meme
`espressif/esp-dsp==1.8.0` des que l'AEC ou l'AFE est active. Le fichier
d'exemple ajoute en plus l'override au niveau projet (priorite finale) :

```yaml
esp32:
  framework:
    type: esp-idf
    components:
      - espressif/esp-dsp==1.8.0
    advanced:
      enable_idf_experimental_features: true
```

Cet alignement permet a l'AFE esp-sr et a `micro_wake_word` de coexister.

## 7. Compilation et flash

```bash
esphome run example/fdaudio-s3box3-duplex-test.yaml
```

Notes :

- `compile_process_limit: 1` est conseille (les piles esp-dl et esp-sr rendent
  la compilation lourde et peuvent provoquer un arret memoire "cc1plus Killed").
- La PSRAM octale est requise par l'AFE ; le fichier d'exemple active
  `CONFIG_SPIRAM_MODE_OCT`, `CONFIG_SPIRAM_SPEED_80M` et
  `CONFIG_SPIRAM_USE_MALLOC`.
- Le watchdog de tache est desactive au demarrage, car l'initialisation esp-sr
  peut bloquer la loopTask quelques secondes.

## 8. Validation du full-duplex

1. Lancer l'assistant vocal.
2. Pendant la reponse vocale (TTS), prononcer le mot d'eveil.
3. Interpretation :
   - l'appareil vous entend pendant qu'il parle : full-duplex operationnel ;
   - il ne vous entend que lorsque le haut-parleur se tait : l'AEC ou le duplex
     doit etre ajuste.

Les premieres lectures du microphone sont tracees au niveau INFO :

```
mic read #1: ret=0 raw_peak=1234 (read 1536 codec samples)
```

Un `raw_peak` proche de 0 indique un microphone muet (voir le depannage).

## 9. Depannage

| Symptome | Cause probable | Action |
| -------- | -------------- | ------ |
| Plus de son ni micro des que AEC/AFE est active | Versions esp-dsp incompatibles entre esp-sr et esp-dl | Verifier l'epingle `esp-dsp==1.8.0` (automatique cote composant ; presente aussi dans l'exemple). |
| Aucun son alors que le micro fonctionne | PA non active | S'assurer que GPIO46 est mis a l'etat haut au boot (`output.turn_on`). |
| Microphone quasi muet (`raw_peak` proche de 0) | Mauvais canal ES7210 | Essayer `mic_channels: 2`, `4`, ou `3` selon le cablage. |
| Microphone trop faible pour le mot d'eveil | Gain insuffisant | Augmenter `mic_gain_db`, ou utiliser `mic_digital_gain` / `mic_agc`. |
| L'AEC coupe le debut des phrases | Fenetre de gating trop courte | Augmenter `aec_gate_ms` (350 a 500). |
| Echo residuel juste apres l'arret du HP | Fenetre de gating trop longue | Diminuer `aec_gate_ms` (150 a 200). |
| Son hache quand l'AFE tourne | Contention CPU sur le coeur 1 | Verifier que l'AFE est bien sur le coeur 0 (place par defaut). |
| Compilation arretee ("cc1plus Killed") | Memoire insuffisante a la compilation | Conserver `compile_process_limit: 1`. |

## 10. Ressources et compromis

| Chemin | Flash | RAM / PSRAM | CPU |
| ------ | ----- | ----------- | --- |
| Sans AEC | minimal | minimal | minimal |
| AEC simple | quelques centaines de Ko | quelques Ko | modere (en ligne) |
| AFE complete | de l'ordre de 1 Mo | plusieurs centaines de Ko en PSRAM | significatif (coeur 0) |

Recommandation : pour un full-duplex avec barge-in fiable a moindre cout,
commencer par `enable_aec: true` et `use_afe: false`, puis ne passer a l'AFE que
si l'echo residuel reste genant.
