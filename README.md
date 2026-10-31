# ESP32 FAQ assistant

## What happens

1. `data/faq.txt` is the FAQ source file on the Mac.
2. The text-only Arduino firmware stores the FAQ on the ESP32 and accepts typed
   questions from `mac_client.py` over Wi-Fi.
3. The separate `voice_firmware` target also stores the FAQ on the ESP32. It
   accepts microphone audio from `mac_voice_client.py` and performs wake-word
   and configured-phrase recognition on the ESP32. It can also synthesize
   English text to a WAV file on the ESP32.
4. The Mac can play synthesized audio through `/speak` or automatically speak
   each FAQ answer during a conversation started with `/begin`.

## Run it

1. Connect the ESP32 and, from this project folder, flash the text-only Arduino
   firmware:

   ```sh
   ~/.platformio/penv/bin/platformio run --environment esp32-s3-devkitc-1 --target upload
   ```

   To upload the FAQ filesystem after changing `data/faq.txt`, run:

   ```sh
   ~/.platformio/penv/bin/platformio run --environment esp32-s3-devkitc-1 --target uploadfs
   ```

2. Whenever you change `data/faq.txt`, upload the filesystem image using the
   command above or **PlatformIO: Upload Filesystem Image** in VS Code.
3. Open the serial monitor at **115200 baud** and note the ESP32's IP address.
4. Connect the Mac to the same 2.4 GHz hotspot as the ESP32.
5. From this project folder, run:

   ```sh
   python3 mac_client.py
   ```

   The current board address (`172.20.10.3`) is the default. If it changes,
   pass the new address, for example `python3 mac_client.py http://192.168.1.42`.
   Type a question and press Enter. Commands start with `/`:

   - `/help` lists commands.
   - `/health` checks the ESP32.
   - `/ask <question>` explicitly sends a question.
   - `/exit` or `/quit` exits the Mac client only; it does not turn off the ESP32.

   Plain `exit` is treated like any other question.

If LittleFS reports a corrupted filesystem, the Arduino firmware retries the
mount after formatting the filesystem partition. Formatting erases its
contents, so run **PlatformIO: Upload Filesystem Image** afterward to restore
`data/faq.txt`.

The hotspot name and password are in the local, Git-ignored file
`include/wifi_secrets.h`. They are compiled into the firmware. Keep the ESP32
on a trusted network: the local HTTP endpoint does not require a password.

## Test the connection

With the Mac on the hotspot, check the board's address:

```sh
curl http://ESP32-IP/health
```

Ask a question:

```sh
curl -X POST http://ESP32-IP/ask \
  -H 'Content-Type: text/plain' \
  --data 'What can you do?'
```

Both commands run locally. No internet service is used.

## Edit the FAQ

Each non-comment line in `data/faq.txt` has this format:

```text
id|question|answer|search keywords
```

Add alternate terms and phrasings in the last field, then upload the
filesystem image again. Questions and answers are stored as text, not audio.
The ESP32 searches the file locally and returns a best match, asks for a
rephrase when results are close, or says it does not know.

The bundled FAQ currently contains 1,109 question entries: 108 concise HMI and
technology topics, each with several phrasings, plus the original assistant and
small-talk entries. Alternate phrasings share the same answer; they improve
text matching but do not add new knowledge. This is a curated reference, not a
general-purpose encyclopedia, and it only answers topics represented in the
file. The ESP32 flash has room for more text, but new answers still need to be
supplied and uploaded.

## Voice prototype

The separate `voice_firmware` project adds an ESP-IDF/ESP-SR speech-recognition
path while leaving the Arduino text assistant intact. The Mac can record a
single WAV with `/record`, or continuously stream microphone audio to the ESP32
with `/begin`. In streaming mode the ESP32 listens for the configured
“Hi ESP” wake word, then accepts multiple FAQ questions without requiring the
wake word again for each one. The conversation returns to wake-word mode after
15 seconds without speech. The voice firmware caps the speech model at 192
phrases (below ESP-SR's 200-command limit), registering one primary phrase for
each FAQ topic and then adding a few common “explain,” “what does,” and
“summarize” phrasings for frequently used topics.
This avoids filling the speech model with near-duplicate entries and makes
later FAQ topics, including prototypes and WCAG, available by voice. Less
common glossary variants remain available through the text-only FAQ. Speech
recognition matches configured phrases; it is not arbitrary transcription,
so a spoken question must use one of the supported phrasings.

To try the voice prototype:

1. Install the Mac microphone dependency:

   ```sh
   python3 -m pip install -r requirements-voice.txt
   ```

2. Connect the ESP32 and, from this project folder, flash the voice firmware
   and its FAQ image:

   ```sh
   ~/.platformio/penv/bin/platformio run --project-dir voice_firmware --environment esp32-s3-voice --target upload
   ```

   Alternatively, in VS Code select the `esp32-s3-voice` PlatformIO
   environment and run **PlatformIO: Upload**, or run **Tasks: Run Task** →
   **Upload ESP32-S3 voice firmware**. This separate ESP-IDF firmware replaces
   the Arduino firmware on the board; uploading the Arduino target restores
   the text-only firmware.
3. Open the serial monitor at **115200 baud** and wait for the ESP32 IP and
   voice API readiness message.
4. Run the Mac voice client:

   ```sh
   python3 mac_voice_client.py
   ```

   Use `/health` to check the board, `/begin` for continuous microphone
   streaming with spoken answers, `/record` for a single five-second recording,
   `/record 8` to record for eight seconds, or `/speak Hello there` to test
   English synthesis on the ESP32 and play it through the Mac speakers. While
   streaming, say “Hi ESP” once to start a conversation, then ask follow-up
   questions without repeating the wake word. Press Ctrl-C to stop streaming.
   Pass a new board address as the optional first argument if DHCP assigned a
   different IP.

The continuous audio link uses TCP port 3333 on the local network. The ESP32
returns recognized text and FAQ answers to the client and streams each
synthesized WAV back to the Mac for playback. While the ESP32 synthesizes and
sends a reply, the Mac pauses microphone transmission and discards buffered mic
frames before listening again; this prevents TCP backpressure and speaker
echo. The `/speak` HTTP endpoint is also available for standalone tests.
English speech uses the Flite
`cmu_us_kal` voice on the ESP32; the Mac only plays the returned audio. The
synthesized voice is compact and synthetic, not neural or natural-sounding.
Microphone capture uses a small bounded queue so slow processing does not crash
the audio stream; if it falls behind, the client drops the oldest buffered
frames. No cloud service is involved.

The Flite source and its `COPYING`/`ACKNOWLEDGEMENTS` notices are included in
`voice_firmware/components/flite`. Its small English voice uses about 2.34 MiB
of program flash and 16.6 KiB of dynamic memory in the upstream ESP32 estimate.
The current app build uses 3,377,765 of 5,242,880 bytes, leaving about 1.78 MiB
in the app partition; the firmware build reports 51,040 of 327,680 bytes of
internal RAM. The board boot log confirmed 8 MiB of PSRAM, but its free amount
after model initialization has not been measured.

### Voice troubleshooting

- If `/record` returns HTTP 404 with `Not found. Use GET /health or POST /ask`,
  the text-only Arduino firmware is running. Select the `esp32-s3-voice`
  PlatformIO environment and upload the voice firmware again.
- If the client reports `Network is unreachable`, connect the Mac to the same
  Wi-Fi hotspot as the ESP32. Check the ESP32's serial output for its current
  IP address; it may have changed.
- A stream greeting such as `READY 16000 512 1 Hi ESP` is expected. The client
  uses it to negotiate the audio format and wake phrase.
- Use `/speak Hello there` to check English synthesis and Mac speaker playback
  separately from microphone recognition.
- During `/begin`, `Receiving speech audio (N bytes)` is only the frame header.
  The client now reports received/expected byte counts and stops with a clear
  error if audio transfer stalls for 10 seconds. Stop the stream before testing
  `/speak` so the standalone test does not compete with an active conversation.
- If an answer appears as text but is not spoken, check the client output for
  an audio playback or TTS error and verify Mac microphone/speaker permissions.
- If the client reports that it discarded stale microphone frames, reduce
  background CPU/audio load or move the Mac closer to the access point.
