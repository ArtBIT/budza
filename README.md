# Budž

**Budž** is "nabudženi bedž" a wearable digital badge built on the **Waveshare ESP32-S3-Touch-LCD-1.28** — a 1.28″ round 240×240 IPS display with capacitive touch. Swipe through JPEG stills and looping MJPEG videos. Double-tap to sleep/wake.

TLDR; 
Buy [Waveshare ESP32-S3-Touch-LCD-1.28](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.28) and install firmware **[directly from your browser →](https://artbit.github.io/budz/)** — no software needed, works in Chrome and Edge.

## Bill of Materials

| Qty | Part | Notes |
|-----|------|-------|
| 1 | [Waveshare ESP32-S3-Touch-LCD-1.28](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.28) | Main board — includes display, touch, and IMU |
| 1 | LiPo battery, 3.7 V, JST 1.25 mm (or a small power bank) | Capacity to taste; board has onboard charging via USB-C |
| 1 | USB-C cable | Programming and charging |
| 1 | Pin badge back / enclosure | 3D print or fabricate to suit |
| 1 | Li-Po battery 105 mAh, 3.7 V, micro JST/SPA connector *(optional)* | ~$4 — board has onboard USB-C charging |

## Features

- JPEG still images — displayed until swiped
- MJPEG AVI video — loops until swiped
- Swipe left / right to navigate between items
- Swipe down for media info overlay (filename, resolution, file size, deletion)
- Swipe up for configuration (local WiFi for uploading media, brightness, app info)
- Double-tap anywhere to sleep (backlight off); double-tap to wake
- Media sorted alphabetically
- ~13 MB LittleFS partition for media storage

## Gestures

| Gesture | Action |
|---------|--------|
| Swipe left | Next item |
| Swipe right | Previous item |
| Swipe down | Media info overlay (filename, resolution, file size, deletion) |
| Swipe up | Configuration (local WiFi for uploading media, brightness, app info) |
| Double tap | Sleep / wake |

---

## Setup

### 1. Arduino IDE board settings

| Setting | Value |
|---------|-------|
| Board | ESP32S3 Dev Module |
| Flash Size | 16MB (128Mb) |
| Partition Scheme | 16MB Flash (3MB APP/9MB SPIFFS) |
| USB Mode | USB-CDC and JTAG |
| Upload Mode | UART0 / Hardware CDC |

The partition scheme is defined in `budz/partitions.csv` (3 MB app + 9.4 MB LittleFS).
Arduino IDE picks it up automatically when the sketch folder contains a `partitions.csv` —
no manual installation needed. Just select **Partition Scheme → Custom** in the Tools menu,
or add an explicit entry to `boards.txt` so it appears with a friendly name:

```
esp32s3.menu.PartitionScheme.app3M_spiffs9M_fact512k_16MB=16MB Flash (3MB APP/9MB SPIFFS)
esp32s3.menu.PartitionScheme.app3M_spiffs9M_fact512k_16MB.build.partitions=app3M_spiffs9M_fact512k_16MB
esp32s3.menu.PartitionScheme.app3M_spiffs9M_fact512k_16MB.upload.maximum_size=3145728
```

(`boards.txt` lives at `~/.arduino15/packages/esp32/hardware/esp32/<version>/boards.txt`.
Click **Tools → Reload Board Data** in Arduino IDE 2 after editing.)

### 2. Libraries

Download the official Waveshare demo package — it includes pre-configured versions of
all required libraries:

- **Wiki:** https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.28#Resources
- **Direct download:** https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.28/ESP32-S3-Touch-LCD-1.28-Demo.zip

Extract the zip and copy its `libraries/` folder into your Arduino sketchbook library
path (usually `~/Arduino/libraries/`). The zip includes TFT_eSPI, CST816S, and JPEGDEC
already configured for this board.

**Critical patch required for arduino-esp32 3.x (ESP-IDF 5.x):**

After extracting, open
`~/Arduino/libraries/TFT_eSPI/Processors/TFT_eSPI_ESP32_S3.h` and change:

```cpp
// Before
#elif CONFIG_IDF_TARGET_ESP32S3
    #define SPI_PORT FSPI

// After
#elif CONFIG_IDF_TARGET_ESP32S3
    #define SPI_PORT 2
```

`FSPI = 1` in ESP-IDF 5.x causes `REG_SPI_BASE(1)` to return `0`, crashing at
address `0x10` on the first SPI write. Using `2` maps to the correct hardware register.

The `budz/User_Setup.h` in this repo overrides TFT_eSPI's pin configuration
automatically — no other changes to the extracted library files are needed.

### 3. Compile and upload

Open `budz/budz.ino` in Arduino IDE. Select the board settings above and upload.

**First upload:** the device enters download mode automatically only if your USB-to-serial
adapter supports RTS/DTR reset. With native USB CDC (ttyACM0), hold **BOOT**, press
**RESET**, release **BOOT**, then click Upload.

**Serial monitor:** Arduino IDE's built-in monitor asserts DTR and forces the device into
download mode. Use `screen` instead:

```bash
sudo screen /dev/ttyACM0 115200
```

---

## Preparing media

All media lives in a local `media/` directory and is flashed to LittleFS with
`upload_media.sh`. Items are displayed in alphabetical filename order.

### Images (JPEG)

The display is 240×240. Any JPEG that fits in available SRAM (~300 KB free) works.
Render or export your image, then convert to JPEG:

```bash
# Single file
convert image.png -quality 90 -resize 240x240 image.jpg

# Batch convert a folder of PNGs
for f in *.png; do
    convert "$f" -quality 90 -resize 240x240 "${f%.png}.jpg"
done
```

**Baseline JPEG required.** JPEGDEC does not support progressive JPEGs — they display
as a black screen with no error. Many tools (Photoshop, GIMP, some ImageMagick presets)
save progressive by default. Convert to baseline before uploading:

```bash
# Check encoding (Line = progressive, None = baseline)
identify -verbose image.jpg | grep Interlace

# Fix a single file (lossless)
jpegtran -optimize image.jpg > fixed.jpg

# Fix all JPEGs in media/ in place
for f in media/*.jpg media/*.jpeg; do
    jpegtran -optimize "$f" > /tmp/_jt.jpg && mv /tmp/_jt.jpg "$f"
done
```

`jpegtran` is lossless and rewrites to baseline by default.
Install with: `sudo apt install libjpeg-turbo-progs`

`upload_media.sh` runs this pass automatically before packaging.

### Videos (MJPEG AVI)

The firmware plays MJPEG AVI files using a built-in RIFF parser. Videos loop
automatically. Use the included `dir2mjpeg` script to convert a Blender PNG render
sequence into a compatible AVI.

**Blender render settings:**

- Output format: PNG
- Frame range: whatever you need
- Output path: a dedicated folder, e.g. `render/myclip/`
- Frame filename: default Blender zero-padded 4-digit numbers (`0001.png`, `0002.png`, …)

**Convert to AVI:**

```bash

ffmpeg -y -framerate 24 -i "$INPUT_VIDEO" \
  -c:v mjpeg -q:v 7 -pix_fmt yuvj420p \
  -vf "scale=240:240:flags=lanczos" \
  -an "$OUTPUT_FILE"
```

There is also a helper script `dir2mjpeg` that wraps this command and applies sane defaults for quality, fps, and size:

```bash
# Usage: dir2mjpeg <render_dir> [quality 1-31] [fps] [size]
#   quality: 1 = best, 31 = worst  (default 7)
#   fps:     frames per second      (default 24)
#   size:    output pixel size      (default 240, produces 240×240)

./dir2mjpeg render/myclip
./dir2mjpeg render/myclip 5 24 240   # explicit args
```

Output: `myclip.avi` in the current directory, scaled to 240×240, MJPEG compressed.
The firmware allocates a 64 KB decode buffer per frame; keep JPEG quality moderate
(quality 5–10) so individual frames stay under that limit.

### Flash media to the device

```bash
# Usage: ./upload_media.sh [media_dir] [port]
./upload_media.sh ./media /dev/ttyACM0
```

Put the device into download mode first (hold BOOT + press RESET), then run the script.
It will:
1. Package everything in `media/` folder into a LittleFS image with `mklittlefs`
2. Flash it to the filesystem partition at offset `0x610000`

Only `.jpg`, `.jpeg`, and `.avi` files are shown by the firmware; other files in the
folder are packaged but ignored.

---

## Project structure

```
badge/
  badge.ino          — main firmware
  User_Setup.h       — TFT_eSPI pin configuration (overrides library defaults)

media/               — put your JPEGs and AVIs here (not committed to git)

dir2mjpeg            — converts Blender PNG sequences to MJPEG AVI
upload_media.sh      — packages and flashes the media/ folder to LittleFS

libraries/           — not included in this repo; extract from Waveshare demo package: https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.28#Resources
  TFT_eSPI/          — display driver (contains ESP-IDF 5.x patch)
  CST816S/           — touch controller driver
  JPEGDEC/           — JPEG decoder
```

## Pin reference

Defined in `budz/budz.ino` — matches the Waveshare ESP32-S3-Touch-LCD-1.28 schematic.

| Signal | GPIO |
|--------|------|
| TFT MOSI | 11 |
| TFT SCLK | 10 |
| TFT CS | 9 |
| TFT DC | 8 |
| TFT RST | 14 |
| TFT BL | 2 |
| Touch SDA | 6 |
| Touch SCL | 7 |
| Touch INT | 5 |
| Touch RST | 13 |
