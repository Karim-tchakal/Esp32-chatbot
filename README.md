# ESP32 FAQ Assistant

An offline question-answering assistant for the **ESP32-S3**. Its answers come
from a plain-text FAQ file stored in the board's flash memory. There is no
cloud service, no internet requirement and no language model: the assistant
compares what you ask with the questions and keywords in its FAQ and replies
with the stored answer.

You can ask by **typing** (text assistant) or by **speaking** (voice
assistant). In voice mode the ESP32 also speaks each answer using an on-device
text-to-speech engine.

## Contents

- [Overview](#overview)
- [How the pieces fit together](#how-the-pieces-fit-together)
- [What you need](#what-you-need)
- [Repository layout](#repository-layout)
- [Quick start](#quick-start)
- [The FAQ file](#the-faq-file)
- [Text assistant internals](#text-assistant-internals)
- [Voice assistant internals](#voice-assistant-internals)
- [Computer-side clients](#computer-side-clients)
- [Flash layout](#flash-layout)
- [Configuration reference](#configuration-reference)
- [Testing](#testing)
- [Troubleshooting](#troubleshooting)
- [Limitations and known behaviors](#limitations-and-known-behaviors)
- [Security notes](#security-notes)
- [Third-party components](#third-party-components)
- [Resource usage recorded by the author](#resource-usage-recorded-by-the-author)

## Overview

The repository holds **two independent firmware projects** that share one FAQ
file, `data/faq.txt`. Only one of them runs on the board at a time.

| | Text assistant | Voice assistant |
|---|---|---|
| Location | repository root (`src/main.cpp`) | `voice_firmware/` |
| Framework | Arduino, built with PlatformIO | ESP-IDF 6.0.1, built with PlatformIO |
| PlatformIO environment | `esp32-s3-devkitc-1` | `esp32-s3-voice` |
| How you ask | typed question over USB serial or Wi-Fi HTTP | spoken question streamed from a computer's microphone |
| How it finds an answer | fuzzy text search (`include/faq_search.h`) | ESP-SR wake word and phrase recognition |
| What you get back | answer text | answer text plus synthesized speech |
| Computer-side client | `mac_client.py` | `mac_voice_client.py` |
| FAQ storage | LittleFS (`/faq.txt`) | SPIFFS (`/spiffs/faq.txt`) |

What it is **not**: a general-purpose chatbot. It only answers topics that are
present in the FAQ file. In voice mode it does not transcribe arbitrary
speech; it recognizes a fixed list of configured phrases, so a spoken question
must use one of the supported phrasings.

The bundled FAQ contains 1,109 entries: 108 concise HMI (human-machine
interaction) and technology topics, each with ten phrasings, plus 29 original
assistant and small-talk entries.

## How the pieces fit together

The firmware does not drive any audio hardware: no microphone or speaker is
attached to the ESP32 in this design. A computer captures microphone audio and
plays speech; the ESP32 does the recognition, FAQ lookup and speech synthesis.

```text
TEXT ASSISTANT (Arduino firmware, repository root)

  mac_client.py / curl ---- HTTP POST /ask (plain text) ---->  ESP32-S3, port 80
                       <---- answer as plain text ----------   faq_search.h scans /faq.txt (LittleFS)
  USB serial terminal  <---- typed questions, ANSWER: ... --->  (serial is handled in the same loop)


VOICE ASSISTANT (ESP-IDF firmware, voice_firmware/)

  Computer                                         ESP32-S3
  --------                                         --------
  microphone -> sounddevice
        |  TCP port 3333: raw 16 kHz mono PCM  ->  ESP-SR front end (noise suppression, VAD)
        |                                            |-- WakeNet wake word "Hi ESP"
        |                                            '-- MultiNet: configured FAQ phrases
        |                                                  |
        |                                            FAQ entry (SPIFFS /faq.txt)
        |                                                  |
  speakers <- sounddevice <- text lines + WAV audio <- Flite text-to-speech (cmu_us_kal voice)

  HTTP on port 80 is also available: GET /health, POST /voice (one WAV), POST /speak (text to WAV)
```

## What you need

**Hardware**

- An ESP32-S3 board with 16 MB of flash and 8 MB of octal PSRAM. The project
  targets PlatformIO's `esp32-s3-devkitc-1` board definition with
  `board_upload.flash_size = 16MB` and the `qio_opi` memory type.
- A USB cable for flashing and the serial monitor.
- A computer on the same **2.4 GHz** Wi-Fi network or hotspot as the board
  (the ESP32 does not use 5 GHz).
- For voice mode, a microphone and speakers on that computer.

**Software**

- [PlatformIO](https://platformio.org/) (CLI or the VS Code extension). The
  commands below assume the CLI is at `~/.platformio/penv/bin/platformio`.
- Python **3.10 or newer** (the voice client uses `X | Y` type annotations).
- For voice mode only: the `sounddevice` package (PortAudio), pinned in
  `requirements-voice.txt` as `sounddevice>=0.5,<1`.
- Optional: a C++17 compiler such as `g++` to run the host-side search test.

The clients are named `mac_*` because they were written for macOS. They use
only the Python standard library plus `sounddevice`.

## Repository layout

```text
.
├── README.md
├── platformio.ini               Text-assistant environment (Arduino, esp32-s3-devkitc-1)
├── partitions.csv               Flash layout for the text assistant
├── src/main.cpp                 Text-assistant firmware
├── include/
│   ├── faq_search.h             Header-only fuzzy matcher (no Arduino dependencies)
│   ├── wifi_secrets.h.example   Template for Wi-Fi credentials
│   └── README                   PlatformIO boilerplate
├── data/faq.txt                 FAQ database shared by both firmwares
├── mac_client.py                Text client (HTTP)
├── mac_voice_client.py          Voice client (microphone streaming, playback, HTTP tests)
├── requirements-voice.txt       Python dependency for voice mode
├── test/
│   ├── faq_search_test.cpp      Host-side tests for the matcher and the real FAQ
│   ├── test_mac_client.py       Unit tests for mac_client.py
│   ├── test_mac_voice_client.py Unit tests for mac_voice_client.py
│   └── README                   PlatformIO boilerplate
├── lib/README                   PlatformIO boilerplate (no private libraries)
├── voice_firmware/
│   ├── platformio.ini           Voice environment (ESP-IDF, esp32-s3-voice)
│   ├── CMakeLists.txt           Project file; packs ../data into the SPIFFS image
│   ├── partitions.csv           Flash layout for the voice assistant
│   ├── sdkconfig.defaults       Hand-maintained build options
│   ├── sdkconfig.esp32-s3-voice Generated full configuration (ESP-IDF 6.0.1)
│   ├── dependencies.lock        Resolved component versions
│   ├── main/
│   │   ├── main.cpp             Voice-assistant firmware
│   │   ├── CMakeLists.txt       Component registration
│   │   └── idf_component.yml    Declares the espressif/esp-sr dependency
│   ├── scripts/ensure_idf_python.py   PlatformIO pre-script (see Build system)
│   └── components/flite/        Vendored Flite text-to-speech source
├── .vscode/                     Recommended extension, "Upload ESP32-S3 voice firmware" task
└── .gitignore                   Ignores .pio, managed_components, wifi_secrets.h, __pycache__
```

## Quick start

### Configure Wi-Fi

Both firmwares read the same local, Git-ignored file:

```sh
cp include/wifi_secrets.h.example include/wifi_secrets.h
```

Edit it to set `CHATBOT_WIFI_SSID` and `CHATBOT_WIFI_PASSWORD`. The values are
compiled into the firmware. The voice firmware requires **both** to be
non-empty; the text firmware only requires the SSID.

### Text assistant

1. Connect the ESP32 and, from the project folder, flash the firmware:

   ```sh
   ~/.platformio/penv/bin/platformio run --environment esp32-s3-devkitc-1 --target upload
   ```

2. Upload the FAQ filesystem. Repeat this whenever you change `data/faq.txt`
   (or use **PlatformIO: Upload Filesystem Image** in VS Code):

   ```sh
   ~/.platformio/penv/bin/platformio run --environment esp32-s3-devkitc-1 --target uploadfs
   ```

3. Open the serial monitor at **115200 baud** and note the line
   `Wi-Fi connected. ESP32 IP: ...`.
4. Connect the computer to the same 2.4 GHz hotspot, then run the client:

   ```sh
   python3 mac_client.py
   ```

   The default board address is `http://172.20.10.3`. If your board has a
   different address, pass it as the first argument, for example
   `python3 mac_client.py http://192.168.1.42`. Type a question and press
   Enter. Commands start with `/`:

   - `/help` lists commands.
   - `/health` checks the ESP32.
   - `/ask <question>` explicitly sends a question.
   - `/exit` or `/quit` exits the client only; it does not turn off the ESP32.

   Plain `exit` is treated like any other question.

You can also test without the client. Both commands run locally and use no
internet service:

```sh
curl http://ESP32-IP/health

curl -X POST http://ESP32-IP/ask \
  -H 'Content-Type: text/plain' \
  --data 'What can you do?'
```

You can also type questions directly into the serial monitor.

### Voice assistant

1. Install the microphone dependency on the computer:

   ```sh
   python3 -m pip install -r requirements-voice.txt
   ```

2. Connect the ESP32 and flash the voice firmware. This single command builds
   and flashes the application, the speech models and the FAQ image:

   ```sh
   ~/.platformio/penv/bin/platformio run --project-dir voice_firmware --environment esp32-s3-voice --target upload
   ```

   Alternatively, in VS Code select the `esp32-s3-voice` environment and run
   **PlatformIO: Upload**, or run **Tasks: Run Task** → **Upload ESP32-S3
   voice firmware**. The first build downloads ESP-IDF and the ESP-SR
   component, so it takes a while.

   This firmware **replaces** the text firmware on the board. The two projects
   use different partition tables, so after switching back to the text
   assistant run both the `upload` and `uploadfs` targets there.
3. Open the serial monitor at **115200 baud** and wait for the ESP32 IP address
   and the line `Voice API listening on /voice and /speak`.
4. Run the voice client (pass the board address as the first argument if it
   differs from the default):

   ```sh
   python3 mac_voice_client.py
   ```

   At the `Voice>` prompt:

   | Command | What it does |
   |---|---|
   | `/health` | Checks that the board is ready |
   | `/begin` (alias `/stream`) | Starts continuous microphone streaming with spoken answers |
   | `/record [seconds]` | Records once (default 5 s, maximum 20 s) and sends the WAV to the board |
   | `/speak <text>` | Synthesizes the text on the ESP32 and plays it on the computer |
   | `/help` | Lists commands |
   | `/exit` or `/quit` | Exits the client |

   While streaming, say **“Hi ESP”** once, then ask questions without repeating
   the wake word. Press Ctrl-C to stop streaming and return to the prompt.
   Unlike `mac_client.py`, plain text at this prompt is *not* sent as a
   question; anything that does not start with a known `/command` prints
   `Unknown command`.

## The FAQ file

`data/faq.txt` is plain text, one entry per line:

```text
id|question|answer|search keywords
```

Rules, as enforced by the firmware parsers:

- Exactly **four fields separated by three `|` characters**. A `|` inside a
  field is not supported; lines with the wrong number of separators are skipped
  with a warning.
- `question` and `answer` must be non-empty. `keywords` may be empty.
- Blank lines and lines starting with `#` are ignored. The first line of the
  file is such a comment describing the format.
- Maximum line length: 767 characters for the text firmware and about 2,000 for
  the voice firmware (it reads into a 2,048-byte buffer). Longer lines are
  skipped. The longest bundled line is 261.
- The matcher only recognizes ASCII letters and digits. Keep the file ASCII
  (the bundled file is).
- Answers are returned verbatim. In voice mode they are also spoken, so write
  them as plain, speakable prose (the bundled file writes `data slash faq dot
  txt` and `sixteen megabytes`). An answer must be shorter than 1,024 bytes to
  be synthesized.

The `id` is ignored by the text firmware. The voice firmware uses it to decide
which entries become voice phrases (see
[Which FAQ entries become voice phrases](#which-faq-entries-become-voice-phrases)).

The `search keywords` field is where you add alternate terms, abbreviations and
phrasings. It is used by the text firmware only; the voice firmware ignores it.

### What is in the bundled file

- **29 original entries** with un-numbered ids: what the assistant is and can
  do, how it works offline, the hardware it runs on, small talk (`hello`,
  `thanks`, `goodbye`, ...), and honest "not available" answers such as `time`
  and `weather`.
- **1,080 topic entries** = 108 topics × 10 phrasings. Ids look like
  `wcag_01` … `wcag_10`. Phrasing `_01` is the canonical question; `_02` to
  `_10` follow fixed templates:

  | Suffix | Template |
  |---|---|
  | `_02` | Can you explain X? |
  | `_03` | What does X mean? |
  | `_04` | Define X. |
  | `_05` | Describe X. |
  | `_06` | Tell me about X. |
  | `_07` | How would you explain X? |
  | `_08` | What should I know about X? |
  | `_09` | Give me a short explanation of X. |
  | `_10` | Can you summarize X? |

- All ten phrasings of a topic share the same answer and keywords, with one
  exception: `wcag_01` has its own wording ("WCAG stands for ...") while
  `wcag_02` to `wcag_10` share another.
- 137 distinct answers in total. The file is 220,206 bytes (about 215 KiB).

Alternate phrasings improve matching; they do not add knowledge. The FAQ is a
curated reference and only answers topics it contains.

<details>
<summary>All 108 topics</summary>

hci, hmi, user_interface, user_experience, interaction_design,
user_centered_design, design_thinking, usability, accessibility,
inclusive_design, learnability, memorability, efficiency, effectiveness,
satisfaction, utility, affordance, signifier, mapping, feedback, constraints,
consistency, visibility, user_control, error_prevention, error_recovery,
recognition_recall, flexibility, minimalist_design, real_world_match,
cognitive_load, mental_model, conceptual_model, information_architecture,
navigation, visual_hierarchy, gestalt_proximity, gestalt_similarity,
gestalt_closure, figure_ground, fitts_law, hicks_law, wcag, pour,
keyboard_accessibility, screen_reader, alt_text, color_contrast, touch_targets,
responsive_design, user_research, user_interview, contextual_inquiry,
usability_observation, survey, persona, scenario, task_analysis, journey_map,
user_flow, wireframe, mockup, prototype, low_fidelity, high_fidelity,
iterative_design, usability_testing, ab_testing, heuristic_evaluation,
cognitive_walkthrough, think_aloud, task_success_rate, time_on_task,
error_rate, sus, onboarding, form_design, input_validation, error_message,
notification, multimodal, voice_interface, conversational_interface,
haptic_feedback, microinteraction, dashboard, human_factors, ergonomics,
privacy_by_design, data_minimization, embedded_system, microcontroller, cpu,
ram, flash_memory, esp32_psram, firmware, wifi, database, speech_recognition,
wake_word, text_to_speech, voice_assistant, latency, audio_buffer, llm, api,
http

</details>

### Adding or changing entries

1. Edit `data/faq.txt`.
2. **Text assistant:** run the `uploadfs` target (or **Upload Filesystem
   Image**). **Voice assistant:** re-run the voice `upload` target; it rebuilds
   and flashes the FAQ image together with the models.
3. For voice, remember that only some entries become voice phrases and the
   total is capped; see below.

If the text firmware reports a corrupted LittleFS volume, it retries the mount
after formatting the partition. Formatting erases the FAQ, so upload the
filesystem image again afterward.

## Text assistant internals

Source: `src/main.cpp` and `include/faq_search.h`.

### Startup

1. Opens serial at 115200 baud and prints a banner and whether PSRAM was found.
2. Sets Wi-Fi to station mode with the hostname `esp32-faq`.
3. Mounts LittleFS using the partition labelled `spiffs`. If the mount fails it
   formats the partition and retries.
4. If no SSID is configured it prints how to create `wifi_secrets.h`.
   Otherwise it connects (waiting up to 20 s per attempt, retrying every 10 s
   from the main loop) and starts the HTTP server on port 80 once connected.
5. Prints a `question>` prompt.

The main loop serves HTTP clients and reads serial input at the same time. A
Wi-Fi connection attempt blocks the loop for up to 20 s.

### Serial interface

Type a question and press Enter. The reply is printed as `ANSWER: ...`.
`!help` prints usage. Input is limited to 255 bytes. Warnings and errors are
printed as `WARNING:` and `ERROR:` lines (for example, skipped malformed FAQ
lines or a missing `/faq.txt`).

### HTTP API

| Request | Success | Errors |
|---|---|---|
| `GET /` or `GET /health` | `200` "ESP32 offline FAQ assistant is ready." | `503` if the FAQ file is unavailable |
| `POST /ask` with the question as the `text/plain` body | `200` with the answer text | `400` missing or empty body; `413` longer than 255 bytes; `503` storage unavailable; `500` FAQ has no usable entries |
| anything else | | `404` "Not found. Use GET /health or POST /ask." |

An answer, a "do not know" reply and a "please rephrase" reply are all `200`
responses; the client sees only the text. Send the header
`Content-Type: text/plain` as in the `curl` example above.

### How a question is matched

For each FAQ line the firmware reads the file sequentially and computes
`score = max(similarity(query, question), similarity(query, keywords))`.
There is no index; the whole file is scanned for every question.

`faq::similarity()` returns 0–100:

**Normalization.** Text is lower-cased. Everything except ASCII letters and
digits becomes a single space (so `Fitts's` becomes `fitts s`). The token `u`
is expanded to `you`. *Content tokens* are the words left after removing
single-character tokens, duplicates and these 29 stop words: a, an, and, are,
as, at, be, been, can, do, does, for, from, how, i, in, is, it, me, of, on, or,
please, the, this, to, was, what, you.

**Scoring**, checked in this order:

| Score | Condition |
|---|---|
| 100 | The whole normalized query appears as whole words inside the candidate. If the query is several words but contains only one content word, and the candidate is not identical, the hit only counts when the word after it in the candidate is a stop word (or there is none). |
| 0 | The query is several words with a single content word, and the candidate contains that word but did not qualify above. This stops `what is usability` from matching `What is the System Usability Scale?`. |
| 90 | The query has 3–13 words and some window of the candidate (one word shorter to one word longer) differs by at most one word-level edit: a word inserted, dropped, or swapped for a word one character away (both words at least 3 characters). This is the main typo tolerance. |
| 0–100 | Otherwise, token overlap: each query content token scores 4 if present exactly, 2 if one character away (one inserted, missing or wrong character; both at least 3 characters), else 0. The result is `100 × points / (4 × tokens) + 10 × matched / tokens`, capped at 80 for queries of four or more words and at 100 overall. A one-word query must match a candidate word exactly or it scores 0. |

**Choosing a reply.** The firmware tracks the best and second-best match,
counting entries with identical answer text as one so that the ten phrasings of
a topic do not compete with each other:

| Condition | Reply |
|---|---|
| best score below **55** | `I don't know that one yet. Please add it to faq.txt.` |
| second best within **8** points of the best | `I found a few close matches. Please rephrase your question. Possible matches: <question 1>; <question 2>` |
| otherwise | the best entry's answer |

### Examples and known behaviors

These results were produced by running `faq_search.h` over the bundled
`data/faq.txt` on a computer with the same selection logic as `src/main.cpp`.
They were not captured on a board.

| Query | Result | Why |
|---|---|---|
| `what can u do` | the capabilities answer | `u` expands to `you`; exact phrase, score 100 |
| `what does wcag stand for` | the WCAG definition | exact phrase, score 100 |
| `explain cognitive load` | the cognitive-load answer | clear winner |
| `how do I bake bread` | "I don't know that one yet..." | no entry scores 55 |
| `what is the capital of france` | "I don't know that one yet..." | nothing matches |
| `what is wcag` | "close matches": *What is PSRAM?* and *What are you?* | 108 FAQ lines tie at 90 by dropping the last word; the lines that contain WCAG score 0 because of the single-content-word rule |
| `tell me about wireframes` | "close matches": *Tell me about human-computer interaction.* and *Tell me about human-machine interaction.* | every `Tell me about X.` entry ties at 90 |
| `what time is it` | "I don't know that one yet..." | the single-content-word rule scores the `time` entry 0. `tell me the time`, `current time` and `time` do match it |
| `who won the game` | the `name` answer | incidental word overlap with that entry's keywords reaches score 56 |

In short: the matcher works best when the query is close to a stored phrasing
or contains distinctive keywords. A query that differs from several stored
phrasings by one word can tie with all of them and trigger the "rephrase"
reply, and an out-of-scope query can occasionally land on a loosely related
answer. Adding the phrasing you expect to the `search keywords` field is the
usual fix.

## Voice assistant internals

Source: `voice_firmware/main/main.cpp` (one file, about 1,200 lines) plus the
vendored Flite component. The voice firmware does **not** use `faq_search.h`:
it recognizes spoken phrases instead of searching text.

### Boot sequence (`app_main`)

If any step fails the firmware logs an error and stops initializing.

1. Initialize NVS (erasing it if it is full or from a newer version).
2. Mount the SPIFFS partition labelled `spiffs` at `/spiffs` (it is never
   auto-formatted) and load the FAQ voice phrases.
3. Initialize speech recognition:
   - Load the models from the `model` partition.
   - Create the English MultiNet model with a 6,000 ms command timeout.
   - For each voice phrase, generate phonemes with Flite's grapheme-to-phoneme
     routine (`flite_g2p`) and register it with
     `esp_mn_commands_phoneme_add`. The command id is the phrase's index in the
     loaded list. Then call `esp_mn_commands_update`.
   - Create the ESP-SR audio front end (`AFE_TYPE_SR`, `AFE_MODE_LOW_COST`,
     single microphone channel, no echo-reference channel) with WakeNet and
     voice-activity detection (VAD) enabled and extra allocations placed in
     PSRAM. It requires 16 kHz, one channel.
4. Initialize the Flite synthesizer with the `cmu_us_kal` voice.
5. Mark the assistant ready, then join Wi-Fi (reconnecting automatically on
   disconnect) and log the IP address.
6. Start the HTTP server (`/health`, `/voice`, `/speak`).
7. Start the audio-stream server task on TCP port 3333 (pinned to core 1,
   8 KiB stack allocated in PSRAM, priority 5).

### Models and ESP-SR configuration

Set in `voice_firmware/sdkconfig.defaults`:

- Wake word: WakeNet9 **“Hi ESP”** (`CONFIG_SR_WN_WN9_HIESP`).
- Phrase recognition: English MultiNet5, single recognition, 8-bit quantized
  (`CONFIG_SR_MN_EN_MULTINET5_SINGLE_RECOGNITION_QUANT8`).
- Octal PSRAM at 80 MHz, 240 MHz CPU, 32 KB instruction cache, 64 KB data cache
  with 64-byte lines, and a 65,535-byte default TCP send buffer.

The generated `sdkconfig.esp32-s3-voice` also records the WebRTC variants of
noise suppression and VAD. Edit `sdkconfig.defaults` rather than the generated
file.

The wake phrase shown to the client (`Hi ESP`) is written into the stream
greeting by the firmware; if you change the WakeNet model you must change that
string as well.

### Which FAQ entries become voice phrases

The speech model holds a limited number of commands, so the firmware does not
register all 1,109 entries. `loadFaq()` selects as follows:

1. **Primary phrases** (registered first, in file order):
   - every un-numbered id (the 29 original entries) **except `time`** (there is
     no clock), and
   - every `_01` id **except `affordance_01`**.
2. **Extra phrases** (added after all primaries): the `_02`, `_03` and `_10`
   phrasings ("Can you explain X?", "What does X mean?", "Can you summarize
   X?") of a fixed list of 19 frequently used topics: hci, hmi, user_interface,
   user_experience, interaction_design, design_thinking, usability,
   accessibility, wcag, prototype, usability_testing, user_research,
   user_interview, persona, journey_map, wireframe, voice_interface,
   microcontroller, wake_word.
3. The total is capped at **192** phrases (`kMaximumFaqEntries`).
4. Entries whose question is longer than ESP-SR's maximum phrase length are
   skipped with a warning.

With the bundled FAQ this yields 135 primary phrases and 57 extras. Run
against the bundled file, the loader logic also selects the three
`usability_observation_*` extras, because that id starts with the `usability_`
alias prefix, and the 192 cap then drops the last three extra candidates:
`voice_interface_10`, `microcontroller_10` and `wake_word_10`. (This was
determined by simulating the selection code on the FAQ file, not by running on
a board.)

If you add primary phrases, they consume slots from the extras; once the 135
primaries plus any new ones reach 192, later primaries are dropped.

### Streaming conversation

The computer connects to TCP port 3333 and sends audio frames continuously; the
ESP32 runs this state machine:

| State | What happens | Event sent |
|---|---|---|
| Idle | Frames go through the ESP-SR front end; the firmware waits for the wake word. | `EVENT WAKE_REQUIRED` (on connect and on timeout) |
| Wake word heard | The firmware clears the recognizer and waits for VAD to report silence, so the wake word itself is not fed to phrase recognition. | `EVENT WAKE` |
| Listening | Noise-suppressed audio is buffered and fed to MultiNet in model-sized chunks. When a phrase is recognized the FAQ text is sent, then the spoken answer. | `EVENT LISTENING`, then the reply (below) |
| Timeout | After 15 s with no speech the conversation ends and the wake word is required again. | `EVENT WAKE_REQUIRED` |

One connection is served at a time (the server accepts clients sequentially).

### Stream protocol (TCP port 3333)

**Computer → ESP32:** raw audio, nothing else — 16-bit little-endian mono PCM
at 16 kHz, in frames of exactly the size announced in the greeting.

**ESP32 → computer:** newline-terminated UTF-8 lines, plus length-prefixed
audio:

| Message | Meaning |
|---|---|
| `READY 16000 <frame samples> 1 Hi ESP` | Greeting. Fields: sample rate, samples per frame, channels, wake phrase. The frame size comes from the ESP-SR front end; the bundled client tests use `512` (1,024 bytes, 32 ms). |
| `EVENT WAKE_REQUIRED` / `EVENT WAKE` / `EVENT LISTENING` | Conversation state changes (see above). |
| `Recognized: <phrase>` followed by `Answer: <answer>` | A phrase was recognized. `<phrase>` is the configured FAQ question that matched, not a transcript of what was said. |
| `EVENT SPEAKING` | The ESP32 is synthesizing the answer. |
| `AUDIO <n>` followed by exactly `<n>` bytes | A complete WAV file (44-byte header plus PCM). |
| `EVENT TTS_ERROR <message>` | Synthesis failed or the answer was too long. |
| `ERROR <message>` | The stream could not start (for example, an allocation failure). |

### HTTP API (voice firmware)

| Request | Success | Errors |
|---|---|---|
| `GET /health` | `200` "ESP32 voice FAQ assistant is ready." | `503` if not ready |
| `POST /voice` with a PCM WAV body (mono, 16 kHz, 16-bit, 44 bytes to 640 KiB, about 20 s) | `200` text `Recognized: <phrase>` + `Answer: <answer>`, or "I could not recognize a configured FAQ question." | `413` size out of range; `415` wrong format; `408` incomplete upload; `500` not enough PSRAM; `503` not ready |
| `POST /speak` with `text/plain` of 1–1,023 bytes | `200` `audio/wav`, sent in chunks | `400` bad length; `408` incomplete; `500` synthesis failed; `503` not ready |

`/voice` runs phrase recognition on the uploaded audio directly; it does not use
the wake word. There is no `/ask` endpoint on the voice firmware.

### Speech synthesis

Synthesis uses **Flite** with the `cmu_us_kal` voice, English only. The voice
database is 8,000 Hz, so synthesized WAVs are 8 kHz mono 16-bit; the client
plays them at whatever rate the WAV header declares. `synthesizeUtterance()`
runs Flite's tokenizer, text analysis, part-of-speech tagging, phrasing,
lexical insertion, pause insertion, intonation and duration stages, and the
registered voice supplies the remaining stages. A mutex serializes access to
the synthesizer. Audio is sent in 1 KiB chunks copied through an internal-RAM
buffer. The voice is compact and synthetic, not neural or natural-sounding.

### Build system

- `voice_firmware/platformio.ini` sets `src_dir = main` and uses the `espidf`
  framework on the `esp32-s3-devkitc-1` board with 16 MB flash.
- `CMakeLists.txt` calls `spiffs_create_partition_image(spiffs ../data
  FLASH_IN_PROJECT)`, so the **entire `data/` folder** is packed into the FAQ
  image.
- `main/idf_component.yml` pulls `espressif/esp-sr` `~2.4.7` from the ESP
  component registry. `dependencies.lock` pins the resolved set (esp-sr 2.4.7,
  cjson 1.7.19~2, dl_fft 0.8.0, esp-dsp 1.8.0, ESP-IDF 6.0.1). The downloaded
  `managed_components/` folder is Git-ignored.
- `scripts/ensure_idf_python.py` is a PlatformIO pre-script. It puts
  PlatformIO's ESP-IDF Python environment on `PATH` (and fails with a clear
  message if it has not been installed yet, which the first build does). Before
  an upload it builds the `spiffs_spiffs_bin` and `srmodels_bin` targets with
  Ninja and adds two extra images to the flash command: the speech models at
  `0x510000` and the FAQ image at `0xa1c000`.
- `components/flite` is built as an ESP-IDF component. It compiles Flite's core
  sources, the `cmu_us_kal` voice, `cmulex` and `usenglish`, and leaves out the
  operating-system audio, memory-map and socket back ends
  (`CST_NO_SOCKETS`, `CST_NO_MMAP`). Because ESP-SR's G2P library exports
  overlapping Flite symbols, selected functions in `cst_synth.c` are renamed
  with a `flite_tts_` prefix at compile time, and `main.cpp` declares those
  renamed functions itself.

## Computer-side clients

### `mac_client.py` (text)

`python3 mac_client.py [esp32_url] [--timeout SECONDS]`

- Default URL `http://172.20.10.3`; default timeout 30 s.
- Prompt `You> `. Plain text is sent as a question to `POST /ask`. Commands:
  `/help`, `/health`, `/ask <question>`, `/exit` / `/quit`. Unknown slash
  commands are reported and never sent as questions.
- Prints `ESP32> <answer>`, and reports HTTP errors, unreachable hosts and
  invalid UTF-8 without crashing.

### `mac_voice_client.py` (voice)

`python3 mac_voice_client.py [esp32_url] [--seconds SECONDS] [--timeout SECONDS]`

- Default URL `http://172.20.10.3`; default recording length 5 s (maximum 20 s,
  matching the firmware's 640 KiB WAV limit); default timeout 180 s.
- Prompt `Voice> ` with the commands listed in the quick start.
- The stream port is fixed at 3333; only the host is taken from the URL.

How `/begin` works:

1. Connects to port 3333, reads the `READY` greeting and checks that it is
   16 kHz mono.
2. Opens the microphone at 16 kHz mono with a block size equal to the
   announced frame size. A callback puts frames in a bounded queue of 8; if the
   queue is full the oldest frame is dropped and a message is printed.
3. The main loop sends one frame at a time and polls the socket for replies.
   `StreamResponseParser` splits the incoming bytes into text lines and
   `AUDIO` frames, and reports download progress.
4. Received WAVs go to a playback thread. **While the ESP32 is synthesizing and
   while audio is playing, the client stops sending microphone frames and
   discards buffered ones.** This avoids TCP backpressure and the speaker being
   picked up by the microphone.
5. If a speech download makes no progress for 10 s the client stops with an
   error. Ctrl-C stops streaming cleanly.

## Flash layout

Both layouts fill the full 16 MiB.

**Text assistant** (`partitions.csv`)

| Name | Type / subtype | Offset | Size |
|---|---|---|---|
| nvs | data / nvs | `0x9000` | `0x6000` |
| phy_init | data / phy | `0xf000` | `0x1000` |
| factory | app / factory | `0x10000` | `0x400000` (4 MiB) |
| spiffs | data / spiffs | `0x410000` | `0xBF0000` (about 11.94 MiB), formatted as LittleFS |

**Voice assistant** (`voice_firmware/partitions.csv`)

| Name | Type / subtype | Offset | Size |
|---|---|---|---|
| nvs | data / nvs | `0x9000` | `0x6000` |
| phy_init | data / phy | `0xf000` | `0x1000` |
| factory | app / factory | `0x10000` | `0x500000` (5 MiB) |
| model | data / spiffs | `0x510000` | `0x50c000` (about 5.05 MiB), ESP-SR models |
| spiffs | data / spiffs | `0xa1c000` | `0x5e4000` (about 5.89 MiB), FAQ image |

The FAQ file (about 215 KiB) uses roughly 2% of the text assistant's filesystem
and 4% of the voice assistant's.

## Configuration reference

**Text firmware** (`src/main.cpp`)

| Setting | Value |
|---|---|
| Serial speed | 115200 baud |
| HTTP port | 80 |
| Hostname | `esp32-faq` |
| FAQ path | `/faq.txt` |
| Maximum serial or HTTP question | 255 bytes |
| Maximum FAQ line | 767 characters (768-byte buffer) |
| Minimum match score | 55 |
| Ambiguity margin | 8 points |
| Wi-Fi connect timeout / retry interval | 20 s / 10 s |

**Voice firmware** (`voice_firmware/main/main.cpp`)

| Setting | Value |
|---|---|
| Audio stream port | TCP 3333 |
| HTTP port | 80 (ESP-IDF default), 4 URI handlers |
| Expected sample rate | 16,000 Hz, mono, 16-bit |
| Maximum voice phrases | 192 |
| Maximum uploaded WAV | 640 KiB |
| Maximum TTS text | 1,023 bytes |
| Audio transfer chunk | 1,024 bytes |
| Conversation timeout | 15 s |
| MultiNet command timeout | 6,000 ms |
| FAQ line buffer | 2,048 bytes |
| Socket send timeout | 5 s |
| Serial speed | 115200 baud |

**Clients**

| Setting | `mac_client.py` | `mac_voice_client.py` |
|---|---|---|
| Default URL | `http://172.20.10.3` | `http://172.20.10.3` |
| Default timeout | 30 s | 180 s |
| Recording length | not applicable | 5 s default, 20 s maximum |
| Microphone queue | not applicable | 8 frames |
| Speech-download stall timeout | not applicable | 10 s |
| Largest accepted audio frame | not applicable | 2 MiB |

## Testing

Run everything from the repository root. `platformio.ini` does not define a
native test environment, so the C++ test is compiled directly.

**Matcher and FAQ test (C++)**

```sh
g++ -std=c++17 -Iinclude test/faq_search_test.cpp -o /tmp/faq_search_test
/tmp/faq_search_test
```

It prints nothing on success and aborts on a failed `assert`. It checks the
scoring function (exact phrases, `u` → `you`, typo tolerance, stop-word
behavior, the single-keyword rule) and loads the real `data/faq.txt` to confirm
that specific queries select specific entry ids with the same threshold and
margin as the firmware. It must be run from the repository root because it
opens `data/faq.txt` by relative path.

**Client tests (Python)**

```sh
python3 -m unittest discover -s test -p 'test_*.py' -v
```

There are 22 tests: 5 for `mac_client.py` (command handling) and 17 for
`mac_voice_client.py` (WAV encoding, HTTP requests, greeting parsing, the
stream parser with split frames, oversized-frame rejection, the microphone
buffer's drop and discard behavior, and command dispatch). Audio hardware is
mocked, so `sounddevice` does not need to be installed to run them.

There are no automated tests for the firmware itself; it is exercised on a
board.

## Troubleshooting

**Wi-Fi and connection**

- `Wi-Fi is not configured` (text firmware) or `Wi-Fi credentials are missing`
  (voice firmware): create `include/wifi_secrets.h` as described in the quick
  start and reflash.
- If the client reports `Network is unreachable`, connect the computer to the
  same Wi-Fi hotspot as the ESP32. Check the ESP32's serial output for its
  current IP address; DHCP may have changed it.
- If `/record` returns HTTP 404 with `Not found. Use GET /health or POST /ask`,
  the **text** firmware is running. Upload the voice firmware again.

**Text assistant**

- `Could not open /faq.txt. Upload the FAQ filesystem.` means the filesystem
  image has not been uploaded: run the `uploadfs` target.
- `LittleFS mount failed; retrying after formatting its partition` is followed
  by an instruction to upload the filesystem image again.
- `Skipped N malformed or overlong FAQ line(s)` means some lines in `faq.txt`
  do not have exactly four fields or exceed 767 characters.

**Voice assistant**

- A stream greeting such as `READY 16000 512 1 Hi ESP` is expected. The client
  uses it to negotiate the audio format and wake phrase.
- Use `/speak Hello there` to check English synthesis and speaker playback
  separately from microphone recognition. Stop any `/begin` stream first so the
  standalone test does not compete with an active conversation.
- During `/begin`, `Receiving speech audio (N bytes)` is only the frame
  header. The client then reports received/expected byte counts and stops with
  a clear error if the transfer stalls for 10 seconds.
- If an answer appears as text but is not spoken, check the client output for an
  audio playback or TTS error and verify the computer's microphone and speaker
  permissions.
- If the client reports that it discarded stale microphone frames, reduce
  background CPU or audio load, or move the computer closer to the access
  point.
- `PlatformIO's ESP-IDF Python environment is missing`: run the voice target
  build once; it installs that environment.
- `Speech models not found in the model partition` or `Could not mount FAQ
  partition`: flash with the voice `upload` target, which writes the models and
  the FAQ image as well as the application.
- A log line such as `MultiNet rejected N FAQ phrase(s)` or `Could not generate
  phonemes for FAQ phrase` points to specific phrases in `faq.txt` that the
  recognizer would not accept.
- If a spoken question is not recognized, use one of the registered phrasings
  (the `_01` question, or an `_02`/`_03`/`_10` phrasing for the topics listed
  above).

## Limitations and known behaviors

- **Fixed knowledge.** Only topics in `data/faq.txt` can be answered. The
  assistant has no clock, weather source or conversation memory, and it says so
  in its answers.
- **Voice recognizes phrases, not speech.** The voice assistant matches against
  at most 192 configured phrases and replies only when one is recognized.
- **Text matching quirks.** See
  [Examples and known behaviors](#examples-and-known-behaviors): ties between
  entries that share a phrasing template, the single-content-word rule, and
  occasional loose matches for out-of-scope questions.
- **One streaming client at a time.** The voice stream server handles
  connections sequentially.
- **Linear scan.** The text firmware reads the whole FAQ file for each
  question; this is fine for the bundled file but would slow down as it grows.
- **English only**, for both matching and synthesis.
- **Computer required for audio.** The ESP32 has no audio hardware wired in
  this project; the microphone and speaker are the computer's.

## Security notes

- Wi-Fi credentials are compiled into the firmware image; keep
  `include/wifi_secrets.h` out of version control (it is Git-ignored) and be
  careful with firmware binaries.
- The HTTP endpoints and the TCP audio stream have **no authentication or
  encryption**. Anyone on the same network can query the board and stream
  audio to it. Keep the ESP32 on a trusted network.
- Everything runs locally; no data is sent to an internet service.

## Third-party components

- **Flite** (CMU speech synthesis) is vendored in
  `voice_firmware/components/flite`, together with its `COPYING` and
  `ACKNOWLEDGEMENTS` files. `COPYING` describes the license terms (BSD-like for
  the core code, with a few files under other free licenses).
- **ESP-SR** (`espressif/esp-sr` 2.4.7, with its cjson, dl_fft and esp-dsp
  dependencies) is downloaded by the ESP component manager at build time.
- **ESP-IDF** and the **Arduino core** are provided by PlatformIO's
  `espressif32` platform (unpinned in `platformio.ini`; `dependencies.lock` and
  the generated `sdkconfig` record ESP-IDF 6.0.1).
- **sounddevice** (PortAudio bindings) is used by the voice client.

The repository does not include a license file for the project's own code.

## Resource usage recorded by the author

These figures were recorded by the project author from builds and a board boot
log; they are not recomputed here.

- Flite's small English voice uses about 2.34 MiB of program flash and 16.6 KiB
  of dynamic memory in the upstream ESP32 estimate.
- The voice application used 3,377,765 of 5,242,880 bytes of the app partition
  (about 1.78 MiB free) and 51,040 of 327,680 bytes of internal RAM.
- The board boot log confirmed 8 MiB of PSRAM. The amount free after model
  initialization has not been measured.