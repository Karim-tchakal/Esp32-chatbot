#!/usr/bin/env python3
"""Record a short question on the Mac and send its WAV to the ESP32."""

import argparse
import array
import io
import math
import queue
import select
import socket
import sys
import subprocess
import threading
import time
import urllib.error
import urllib.request
import wave
from urllib.parse import urlsplit

DEFAULT_ESP32_URL = "http://172.20.10.2"
SAMPLE_RATE = 16000
MAX_RECORDING_SECONDS = 20
STREAM_PORT = 3333
MAX_STREAM_AUDIO_BYTES = 2 * 1024 * 1024
STREAM_AUDIO_STALL_TIMEOUT = 10.0
MICROPHONE_QUEUE_FRAMES = 8
TTS_SMOOTHING_CUTOFF_HZ = 6200.0
TTS_COMPRESSOR_THRESHOLD = 0.55
TTS_COMPRESSOR_RATIO = 2.0
COMMANDS = (
    "/help, /health, /record [seconds], /speak <text>, /mac, /esp, /begin, /exit"
)


class SpeechSettings:
    def __init__(self) -> None:
        self.backend = "esp32"


def encode_wav(pcm_data: bytes, sample_rate: int = SAMPLE_RATE) -> bytes:
    """Wrap mono, 16-bit PCM samples in a WAV container."""
    wav_buffer = io.BytesIO()
    with wave.open(wav_buffer, "wb") as wav_file:
        wav_file.setnchannels(1)
        wav_file.setsampwidth(2)
        wav_file.setframerate(sample_rate)
        wav_file.writeframes(pcm_data)
    return wav_buffer.getvalue()


def smooth_speech_pcm(pcm_data: bytes, sample_rate: int) -> bytes:
    """Gently soften bright peaks in signed 16-bit mono synthesized speech."""
    if len(pcm_data) % 2:
        raise ValueError("Speech PCM data must contain whole 16-bit samples.")
    if sample_rate <= 0:
        raise ValueError("Speech sample rate must be positive.")
    if not pcm_data:
        return pcm_data

    samples = array.array("h")
    samples.frombytes(pcm_data)
    if samples.itemsize != 2:
        raise RuntimeError("The host does not support 16-bit speech samples.")
    if sys.byteorder != "little":
        samples.byteswap()

    cutoff_hz = min(TTS_SMOOTHING_CUTOFF_HZ, sample_rate * 0.45)
    omega = 2.0 * math.pi * cutoff_hz / sample_rate
    cosine = math.cos(omega)
    alpha = math.sin(omega) / (2.0 * math.sqrt(0.5))
    a0 = 1.0 + alpha
    b0 = (1.0 - cosine) / (2.0 * a0)
    b1 = (1.0 - cosine) / a0
    b2 = b0
    a1 = -2.0 * cosine / a0
    a2 = (1.0 - alpha) / a0

    x1 = x2 = y1 = y2 = 0.0
    envelope = 0.0
    attack = math.exp(-1.0 / (0.01 * sample_rate))
    release = math.exp(-1.0 / (0.08 * sample_rate))
    threshold = TTS_COMPRESSOR_THRESHOLD
    for index, sample in enumerate(samples):
        x0 = sample / 32768.0
        filtered = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1 = x1, x0
        y2, y1 = y1, filtered

        coefficient = attack if abs(filtered) > envelope else release
        envelope = coefficient * envelope + (1.0 - coefficient) * abs(filtered)
        if envelope > threshold:
            target_level = threshold + (envelope - threshold) / TTS_COMPRESSOR_RATIO
            filtered *= target_level / envelope
        samples[index] = max(-32768, min(32767, round(filtered * 32768.0)))

    if sys.byteorder != "little":
        samples.byteswap()
    return samples.tobytes()


def record_audio(seconds: float) -> bytes:
    if seconds <= 0 or seconds > MAX_RECORDING_SECONDS:
        raise ValueError(
            f"Recording length must be between 0 and {MAX_RECORDING_SECONDS} seconds."
        )

    try:
        import sounddevice
    except ImportError as error:
        raise RuntimeError(
            "Audio support is missing. Install it with "
            "`python3 -m pip install -r requirements-voice.txt`."
        ) from error

    frame_count = round(seconds * SAMPLE_RATE)
    block_size = 1024
    remaining = frame_count
    pcm_chunks = []
    with sounddevice.RawInputStream(
        samplerate=SAMPLE_RATE,
        channels=1,
        dtype="int16",
        blocksize=block_size,
    ) as stream:
        while remaining:
            data, overflowed = stream.read(min(block_size, remaining))
            if overflowed:
                raise RuntimeError("Microphone input overflowed; please record again.")
            pcm_chunks.append(data)
            remaining -= min(block_size, remaining)

    return encode_wav(b"".join(pcm_chunks))


def check_health(base_url: str, timeout: float) -> str:
    request = urllib.request.Request(f"{base_url.rstrip('/')}/health")
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read().decode("utf-8").strip()


def ask_by_voice(base_url: str, wav_data: bytes, timeout: float) -> str:
    request = urllib.request.Request(
        f"{base_url.rstrip('/')}/voice",
        data=wav_data,
        headers={"Content-Type": "audio/wav"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read().decode("utf-8").strip()


def request_speech(base_url: str, text: str, timeout: float) -> bytes:
    request = urllib.request.Request(
        f"{base_url.rstrip('/')}/speak",
        data=text.encode("utf-8"),
        headers={"Content-Type": "text/plain; charset=utf-8"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def speak_macos(text: str) -> None:
    if not text.strip():
        return
    subprocess.run(["say", text], check=True)


def extract_answer(response: str) -> str | None:
    for line in response.splitlines():
        if line.startswith("Answer:"):
            answer = line.partition(":")[2].strip()
            return answer or None
    return None


def play_speech(wav_data: bytes) -> None:
    try:
        import sounddevice
    except ImportError as error:
        raise RuntimeError(
            "Audio support is missing. Install it with "
            "`python3 -m pip install -r requirements-voice.txt`."
        ) from error

    with wave.open(io.BytesIO(wav_data), "rb") as wav_file:
        if wav_file.getnchannels() != 1 or wav_file.getsampwidth() != 2:
            raise ValueError("ESP32 returned unsupported speech audio format.")
        sample_rate = wav_file.getframerate()
        if sample_rate <= 0:
            raise ValueError("ESP32 returned an invalid speech sample rate.")
        pcm_data = smooth_speech_pcm(
            wav_file.readframes(wav_file.getnframes()), sample_rate
        )

    with sounddevice.RawOutputStream(
        samplerate=sample_rate,
        channels=1,
        dtype="int16",
    ) as speaker:
        speaker.write(pcm_data)


def read_stream_line(connection: socket.socket) -> str:
    line = bytearray()
    while True:
        byte = connection.recv(1)
        if not byte:
            raise ConnectionError("The ESP32 closed the audio stream.")
        if byte == b"\n":
            return line.decode("utf-8", errors="replace").strip()
        line.extend(byte)


def parse_stream_greeting(greeting: str) -> tuple[int, int, int, str]:
    fields = greeting.split(maxsplit=4)
    if len(fields) != 5 or fields[0] != "READY":
        raise ConnectionError(f"Unexpected ESP32 stream response: {greeting}")
    try:
        sample_rate, frame_samples, channels = map(int, fields[1:4])
    except ValueError as error:
        raise ConnectionError(f"Invalid ESP32 audio format: {greeting}") from error
    if sample_rate != SAMPLE_RATE or channels != 1 or frame_samples <= 0:
        raise ConnectionError(f"Unsupported ESP32 audio format: {greeting}")
    return sample_rate, frame_samples, channels, fields[4]


class StreamResponseParser:
    """Decode newline-delimited messages and length-prefixed WAV frames."""

    def __init__(self) -> None:
        self.pending = bytearray()
        self.audio_remaining: int | None = None
        self.audio_expected: int | None = None
        self.audio_received = 0
        self.audio_chunks: list[bytes] = []

    def feed(self, data: bytes) -> list[tuple[str, str | bytes]]:
        self.pending.extend(data)
        messages: list[tuple[str, str | bytes]] = []
        while self.pending:
            if self.audio_remaining is not None:
                count = min(self.audio_remaining, len(self.pending))
                self.audio_chunks.append(bytes(self.pending[:count]))
                del self.pending[:count]
                self.audio_remaining -= count
                self.audio_received += count
                messages.append((
                    "audio_progress",
                    f"{self.audio_received}/{self.audio_expected}",
                ))
                if self.audio_remaining:
                    break
                messages.append(("audio", b"".join(self.audio_chunks)))
                self.audio_chunks.clear()
                self.audio_remaining = None
                self.audio_expected = None
                self.audio_received = 0
                continue

            newline = self.pending.find(b"\n")
            if newline < 0:
                break
            line = bytes(self.pending[:newline]).decode(
                "utf-8", errors="replace"
            ).strip()
            del self.pending[:newline + 1]
            if line.startswith("AUDIO "):
                fields = line.split()
                if len(fields) != 2:
                    raise ConnectionError(f"Invalid ESP32 audio header: {line}")
                try:
                    audio_size = int(fields[1])
                except ValueError as error:
                    raise ConnectionError(
                        f"Invalid ESP32 audio header: {line}"
                    ) from error
                if not 44 <= audio_size <= MAX_STREAM_AUDIO_BYTES:
                    raise ConnectionError(
                        f"Unsupported ESP32 audio frame size: {audio_size}"
                    )
                self.audio_remaining = audio_size
                self.audio_expected = audio_size
                self.audio_received = 0
                self.audio_chunks.clear()
                messages.append(("audio_start", str(audio_size)))
            else:
                messages.append(("text", line))
        return messages


class MicrophoneFrameBuffer:
    def __init__(self, max_frames: int = MICROPHONE_QUEUE_FRAMES) -> None:
        self.frames: queue.Queue[bytes] = queue.Queue(maxsize=max_frames)
        self.frames_dropped = threading.Event()
        self.input_overflowed = threading.Event()

    def callback(self, audio: bytes, frame_count: int, time_info, status) -> None:
        del frame_count, time_info
        if getattr(status, "input_overflow", False):
            self.input_overflowed.set()
        frame = bytes(audio)
        try:
            self.frames.put_nowait(frame)
        except queue.Full:
            try:
                self.frames.get_nowait()
            except queue.Empty:
                pass
            self.frames_dropped.set()
            self.frames.put_nowait(frame)

    def get(self, timeout: float) -> bytes:
        return self.frames.get(timeout=timeout)

    def discard_pending(self) -> None:
        while True:
            try:
                self.frames.get_nowait()
            except queue.Empty:
                return


def display_stream_message(message: str) -> None:
    if message == "EVENT WAKE_REQUIRED":
        print("ESP32> Say “Hi ESP” to start a conversation.")
    elif message == "EVENT WAKE":
        print("ESP32> Wake word heard. Ask your question.")
    elif message == "EVENT LISTENING":
        print("ESP32> Listening for your question.")
    elif message.startswith("EVENT "):
        print(f"ESP32> {message[6:]}")
    else:
        print(f"ESP32> {message}")


def stream_audio(
    base_url: str, timeout: float, speech_backend: str = "esp32"
) -> None:
    """Stream microphone audio and play answers using the selected voice."""
    try:
        import sounddevice
    except ImportError as error:
        raise RuntimeError(
            "Audio support is missing. Install it with "
            "`python3 -m pip install -r requirements-voice.txt`."
        ) from error

    url = urlsplit(base_url if "://" in base_url else f"http://{base_url}")
    if not url.hostname:
        raise ValueError(f"Invalid ESP32 address: {base_url}")

    with socket.create_connection((url.hostname, STREAM_PORT), timeout=timeout) as connection:
        connection.settimeout(timeout)
        greeting = read_stream_line(connection)
        sample_rate, frame_samples, channels, wake_word = parse_stream_greeting(
            greeting
        )

        print(f"Streaming microphone audio to {url.hostname}. Press Ctrl-C to stop.")
        print(
            f"Say “{wake_word}” once to start; you can ask follow-up questions "
            "without repeating it."
        )
        responses = StreamResponseParser()
        audio_queue: queue.Queue[tuple[str, bytes | str] | None] = queue.Queue()
        speaking = threading.Event()
        mac_speech_active = threading.Event()
        esp_audio_pending = threading.Event()
        playback_lock = threading.Lock()
        microphone_frames = MicrophoneFrameBuffer()
        audio_stall_deadline: float | None = None

        def resume_microphone_if_ready() -> None:
            with playback_lock:
                if (
                    not mac_speech_active.is_set()
                    and not esp_audio_pending.is_set()
                    and audio_queue.empty()
                ):
                    microphone_frames.discard_pending()
                    speaking.clear()

        def play_queued_audio() -> None:
            while True:
                item = audio_queue.get()
                try:
                    if item is None:
                        return
                    backend, speech = item
                    if backend == "macos":
                        if not isinstance(speech, str):
                            raise TypeError("Invalid macOS speech queue item.")
                        speak_macos(speech)
                        mac_speech_active.clear()
                    else:
                        if not isinstance(speech, bytes):
                            raise TypeError("Invalid ESP32 speech queue item.")
                        play_speech(speech)
                except (
                    OSError,
                    RuntimeError,
                    ValueError,
                    wave.Error,
                    subprocess.SubprocessError,
                ) as error:
                    print(f"Could not play speech: {error}")
                finally:
                    if item is not None:
                        backend, _ = item
                        if backend == "macos":
                            mac_speech_active.clear()
                        else:
                            esp_audio_pending.clear()
                        resume_microphone_if_ready()
                    audio_queue.task_done()

        playback_thread = threading.Thread(
            target=play_queued_audio, name="esp32-speech-playback", daemon=True
        )
        playback_thread.start()
        try:
            with sounddevice.RawInputStream(
                samplerate=sample_rate,
                channels=channels,
                dtype="int16",
                blocksize=frame_samples,
                callback=microphone_frames.callback,
            ) as microphone:
                while True:
                    is_speaking = speaking.is_set()
                    if is_speaking:
                        microphone_frames.discard_pending()
                        audio = None
                    else:
                        try:
                            audio = microphone_frames.get(timeout=0.25)
                        except queue.Empty:
                            audio = None

                    if microphone_frames.input_overflowed.is_set():
                        microphone_frames.input_overflowed.clear()
                        if not is_speaking:
                            print("Microphone input overflowed; restarting capture.")
                    if microphone_frames.frames_dropped.is_set():
                        microphone_frames.frames_dropped.clear()
                        if not is_speaking:
                            print("Microphone fell behind; discarded stale audio frames.")

                    if audio is not None and not speaking.is_set():
                        if len(audio) != frame_samples * channels * 2:
                            raise RuntimeError(
                                "Microphone returned an unexpected audio frame size."
                            )
                        connection.sendall(audio)

                    readable, _, _ = select.select(
                        [connection],
                        [],
                        [],
                        0.05 if speaking.is_set() else 0,
                    )
                    while readable:
                        data = connection.recv(4096)
                        if not data:
                            raise ConnectionError("The ESP32 closed the audio stream.")
                        for kind, content in responses.feed(data):
                            if kind == "text":
                                message = str(content)
                                if (
                                    speech_backend == "macos"
                                    and message.startswith("Answer:")
                                ):
                                    answer = message.partition(":")[2].strip()
                                    if answer:
                                        with playback_lock:
                                            speaking.set()
                                            mac_speech_active.set()
                                            esp_audio_pending.set()
                                            audio_queue.put(("macos", answer))
                                        print("Mac> Speaking answer with offline speech.")
                                if message == "EVENT SPEAKING":
                                    with playback_lock:
                                        speaking.set()
                                    if speech_backend == "esp32":
                                        print("ESP32> Synthesizing speech.")
                                elif message.startswith("EVENT TTS_ERROR"):
                                    display_stream_message(message)
                                    esp_audio_pending.clear()
                                    resume_microphone_if_ready()
                                else:
                                    display_stream_message(message)
                            elif kind == "audio_start":
                                with playback_lock:
                                    speaking.set()
                                    esp_audio_pending.set()
                                audio_stall_deadline = (
                                    time.monotonic() + STREAM_AUDIO_STALL_TIMEOUT
                                )
                                if speech_backend == "esp32":
                                    print(
                                        f"ESP32> Receiving speech audio "
                                        f"({content} bytes)."
                                    )
                            elif kind == "audio_progress":
                                audio_stall_deadline = (
                                    time.monotonic() + STREAM_AUDIO_STALL_TIMEOUT
                                )
                                if speech_backend == "esp32":
                                    received, expected = str(content).split("/", 1)
                                    print(
                                        f"\rESP32> Received {received}/{expected} "
                                        "speech bytes",
                                        end="",
                                        flush=True,
                                    )
                            elif kind == "audio":
                                audio_stall_deadline = None
                                if not isinstance(content, bytes):
                                    raise ConnectionError(
                                        "ESP32 sent invalid audio data."
                                    )
                                if speech_backend == "esp32":
                                    print()
                                    print(
                                        f"ESP32> Received {len(content)} bytes "
                                        "of speech audio."
                                    )
                                    with playback_lock:
                                        audio_queue.put(("esp32", content))
                                        speaking.set()
                                else:
                                    esp_audio_pending.clear()
                                    resume_microphone_if_ready()
                        readable, _, _ = select.select([connection], [], [], 0)

                    if (
                        audio_stall_deadline is not None
                        and time.monotonic() >= audio_stall_deadline
                    ):
                        raise TimeoutError(
                            "ESP32 speech transfer stalled after "
                            f"{responses.audio_received}/"
                            f"{responses.audio_expected} bytes."
                        )
        except KeyboardInterrupt:
            print("\nMicrophone stream stopped.")
        finally:
            audio_queue.put(None)
            playback_thread.join(timeout=5.0)


def send_recording(
    base_url: str,
    seconds: float,
    timeout: float,
    speech_backend: str = "esp32",
) -> None:
    try:
        recording = record_audio(seconds)
        print(f"Sending {seconds:g} seconds of audio to the ESP32...")
        response = ask_by_voice(base_url, recording, timeout)
        print(f"ESP32> {response}")
        if speech_backend == "macos":
            answer = extract_answer(response)
            if answer is not None:
                try:
                    speak_macos(answer)
                except (OSError, subprocess.SubprocessError) as error:
                    print(f"Could not speak with macOS offline speech: {error}")
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", errors="replace").strip()
        print(f"ESP32 returned HTTP {error.code}: {detail}")
    except (urllib.error.URLError, TimeoutError) as error:
        print(f"Could not reach the ESP32: {error}")
    except (RuntimeError, ValueError) as error:
        print(f"Could not record audio: {error}")
    except UnicodeDecodeError as error:
        print(f"ESP32 response was not valid UTF-8: {error}")


def send_speech(base_url: str, text: str, timeout: float) -> None:
    try:
        audio = request_speech(base_url, text, timeout)
        play_speech(audio)
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", errors="replace").strip()
        print(f"ESP32 returned HTTP {error.code}: {detail}")
    except (urllib.error.URLError, TimeoutError) as error:
        print(f"Could not reach the ESP32: {error}")
    except (OSError, RuntimeError, ValueError, wave.Error) as error:
        print(f"Could not play ESP32 speech: {error}")


def handle_input(
    line: str,
    base_url: str,
    timeout: float,
    default_seconds: float,
    speech_settings: SpeechSettings | None = None,
) -> bool:
    """Handle a prompt line; return False only when the user requests exit."""
    message = line.strip()
    if not message:
        return True

    command, _, argument = message.partition(" ")
    command = command.lower()
    argument = argument.strip()
    if speech_settings is None:
        speech_settings = SpeechSettings()

    if command in {"/exit", "/quit"}:
        return False
    if command == "/help":
        print(f"Commands: {COMMANDS}")
    elif command == "/mac":
        speech_settings.backend = "macos"
        print("Mac offline speech enabled for FAQ answers.")
    elif command == "/esp":
        speech_settings.backend = "esp32"
        print("ESP32 Flite speech enabled for FAQ answers.")
    elif command == "/health":
        try:
            print(f"ESP32> {check_health(base_url, timeout)}")
        except urllib.error.HTTPError as error:
            detail = error.read().decode("utf-8", errors="replace").strip()
            print(f"ESP32 returned HTTP {error.code}: {detail}")
        except (urllib.error.URLError, TimeoutError) as error:
            print(f"Could not reach the ESP32: {error}")
        except UnicodeDecodeError as error:
            print(f"ESP32 response was not valid UTF-8: {error}")
    elif command == "/record":
        try:
            seconds = float(argument) if argument else default_seconds
        except ValueError:
            print("Usage: /record [seconds]")
            return True
        send_recording(base_url, seconds, timeout, speech_settings.backend)
    elif command == "/speak":
        if not argument:
            print("Usage: /speak <text>")
            return True
        send_speech(base_url, argument, timeout)
    elif command in {"/begin", "/stream"}:
        try:
            stream_audio(base_url, timeout, speech_settings.backend)
        except (OSError, RuntimeError, ValueError) as error:
            print(f"Audio stream failed: {error}")
    else:
        print(f"Unknown command: {command}. Commands: {COMMANDS}")

    return True


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Record an English question and send it to the ESP32."
    )
    parser.add_argument(
        "esp32_url",
        nargs="?",
        default=DEFAULT_ESP32_URL,
        help=f"ESP32 base URL (default: {DEFAULT_ESP32_URL})",
    )
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--timeout", type=float, default=180.0)
    args = parser.parse_args()
    if not 0 < args.seconds <= MAX_RECORDING_SECONDS:
        parser.error(f"--seconds must be between 0 and {MAX_RECORDING_SECONDS}")

    print(f"Connected target: {args.esp32_url}")
    print(f"Commands: {COMMANDS}")
    print(f"Default recording: {args.seconds:g} seconds at {SAMPLE_RATE} Hz")
    print("Use /begin for streaming; /mac selects Mac offline speech, /esp restores ESP32 speech.")
    speech_settings = SpeechSettings()
    while True:
        try:
            line = input("Voice> ")
        except (EOFError, KeyboardInterrupt):
            print()
            return 0
        if not handle_input(
            line, args.esp32_url, args.timeout, args.seconds, speech_settings
        ):
            return 0


if __name__ == "__main__":
    raise SystemExit(main())
