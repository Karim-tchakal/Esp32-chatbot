import io
import importlib.util
import math
import pathlib
import sys
import unittest
import wave
from array import array
from unittest.mock import MagicMock, patch


MODULE_PATH = pathlib.Path(__file__).parents[1] / "mac_voice_client.py"
SPEC = importlib.util.spec_from_file_location("mac_voice_client", MODULE_PATH)
mac_voice_client = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mac_voice_client)


class MacVoiceClientTests(unittest.TestCase):
    @staticmethod
    def make_tone(frequency, amplitude, sample_rate=16000, sample_count=4096):
        samples = array(
            "h",
            (
                round(
                    amplitude
                    * math.sin(2 * math.pi * frequency * index / sample_rate)
                )
                for index in range(sample_count)
            ),
        )
        return samples.tobytes()

    @staticmethod
    def rms(pcm_data):
        samples = array("h")
        samples.frombytes(pcm_data)
        return math.sqrt(sum(sample * sample for sample in samples) / len(samples))

    def test_record_audio_preserves_microphone_samples(self):
        pcm_data = self.make_tone(1000, 2000, sample_count=1024)
        sounddevice = MagicMock()
        stream = sounddevice.RawInputStream.return_value.__enter__.return_value
        stream.read.return_value = (pcm_data, False)

        with patch.dict(sys.modules, {"sounddevice": sounddevice}):
            wav_data = mac_voice_client.record_audio(1024 / 16000)

        with wave.open(io.BytesIO(wav_data), "rb") as wav_file:
            self.assertEqual(wav_file.readframes(1024), pcm_data)

    def test_speech_smoother_softens_high_frequencies_and_preserves_voice_band(self):
        high_tone = self.make_tone(7500, 10000)
        voice_tone = self.make_tone(1000, 10000)

        smoothed_high = mac_voice_client.smooth_speech_pcm(high_tone, 16000)
        smoothed_voice = mac_voice_client.smooth_speech_pcm(voice_tone, 16000)

        self.assertLess(self.rms(smoothed_high[1024:]), self.rms(high_tone[1024:]) * 0.5)
        self.assertGreater(self.rms(smoothed_voice[1024:]), self.rms(voice_tone[1024:]) * 0.9)

    def test_speech_smoother_compresses_peaks_without_clipping(self):
        loud_tone = self.make_tone(1000, 30000)

        smoothed = mac_voice_client.smooth_speech_pcm(loud_tone, 16000)
        samples = array("h")
        samples.frombytes(smoothed)

        self.assertLess(max(abs(sample) for sample in samples), 30000)
        self.assertGreater(self.rms(smoothed), 10000)

    def test_speech_smoother_rejects_invalid_pcm(self):
        with self.assertRaisesRegex(ValueError, "whole 16-bit samples"):
            mac_voice_client.smooth_speech_pcm(b"\x00", 16000)
        with self.assertRaisesRegex(ValueError, "sample rate"):
            mac_voice_client.smooth_speech_pcm(b"\x00\x00", 0)

    def test_encode_wav_creates_expected_pcm_format(self):
        pcm_data = b"\x00\x00\x01\x00"

        encoded = mac_voice_client.encode_wav(pcm_data)

        with wave.open(io.BytesIO(encoded), "rb") as wav_file:
            self.assertEqual(wav_file.getnchannels(), 1)
            self.assertEqual(wav_file.getsampwidth(), 2)
            self.assertEqual(wav_file.getframerate(), 16000)
            self.assertEqual(wav_file.readframes(2), pcm_data)

    def test_voice_endpoint_receives_wav_post(self):
        response = MagicMock()
        response.read.return_value = b"recognized"
        response.__enter__.return_value = response
        with patch.object(mac_voice_client.urllib.request, "urlopen",
                          return_value=response) as urlopen:
            result = mac_voice_client.ask_by_voice(
                "http://esp32.local/", b"wav-data", 12.0
            )

        self.assertEqual(result, "recognized")
        request = urlopen.call_args.args[0]
        self.assertEqual(request.full_url, "http://esp32.local/voice")
        self.assertEqual(request.get_method(), "POST")
        self.assertEqual(request.data, b"wav-data")
        self.assertEqual(request.get_header("Content-type"), "audio/wav")
        self.assertEqual(urlopen.call_args.kwargs["timeout"], 12.0)

    def test_speak_macos_uses_builtin_offline_speech_command(self):
        with patch.object(mac_voice_client.subprocess, "run") as run:
            mac_voice_client.speak_macos("Hello from the Mac")

        run.assert_called_once_with(["say", "Hello from the Mac"], check=True)

    def test_extract_answer_from_voice_response(self):
        self.assertEqual(
            mac_voice_client.extract_answer("Recognized: Hello\nAnswer: Hi there"),
            "Hi there",
        )
        self.assertIsNone(mac_voice_client.extract_answer("No FAQ match"))

    @patch.object(mac_voice_client, "speak_macos")
    @patch.object(mac_voice_client, "ask_by_voice", return_value="Answer: Hello there")
    @patch.object(mac_voice_client, "record_audio", return_value=b"wav-data")
    def test_recording_uses_macos_voice_for_faq_answer(
        self, record_audio, ask_by_voice, speak_macos
    ):
        with patch("builtins.print"):
            mac_voice_client.send_recording(
                "http://esp32.local", 5.0, 30.0, "macos"
            )

        record_audio.assert_called_once_with(5.0)
        ask_by_voice.assert_called_once_with(
            "http://esp32.local", b"wav-data", 30.0
        )
        speak_macos.assert_called_once_with("Hello there")

    def test_speech_endpoint_receives_text_post(self):
        response = MagicMock()
        response.read.return_value = b"wav-data"
        response.__enter__.return_value = response
        with patch.object(mac_voice_client.urllib.request, "urlopen",
                          return_value=response) as urlopen:
            result = mac_voice_client.request_speech(
                "http://esp32.local/", "Hello there", 12.0
            )

        self.assertEqual(result, b"wav-data")
        request = urlopen.call_args.args[0]
        self.assertEqual(request.full_url, "http://esp32.local/speak")
        self.assertEqual(request.get_method(), "POST")
        self.assertEqual(request.data, b"Hello there")
        self.assertEqual(
            request.get_header("Content-type"), "text/plain; charset=utf-8"
        )
        self.assertEqual(urlopen.call_args.kwargs["timeout"], 12.0)

    def test_play_speech_writes_pcm_to_default_speakers(self):
        wav_data = mac_voice_client.encode_wav(b"\x01\x00\x02\x00")
        expected_pcm = mac_voice_client.smooth_speech_pcm(b"\x01\x00\x02\x00", 16000)
        sounddevice = MagicMock()
        speaker = sounddevice.RawOutputStream.return_value.__enter__.return_value
        with patch.dict(sys.modules, {"sounddevice": sounddevice}):
            mac_voice_client.play_speech(wav_data)

        sounddevice.RawOutputStream.assert_called_once_with(
            samplerate=16000, channels=1, dtype="int16"
        )
        speaker.write.assert_called_once_with(expected_pcm)

    def test_stream_greeting_preserves_multiword_wake_word(self):
        greeting = "READY 16000 512 1 Hi ESP"

        result = mac_voice_client.parse_stream_greeting(greeting)

        self.assertEqual(result, (16000, 512, 1, "Hi ESP"))

    def test_stream_greeting_rejects_invalid_audio_format(self):
        with self.assertRaisesRegex(ConnectionError, "Unsupported ESP32 audio format"):
            mac_voice_client.parse_stream_greeting("READY 8000 512 1 Hi ESP")

    def test_stream_response_parser_reads_text_and_wav_frames(self):
        parser = mac_voice_client.StreamResponseParser()
        wav_data = mac_voice_client.encode_wav(b"\x01\x00\x02\x00")

        messages = parser.feed(
            b"Recognized: Hello\nAnswer: Hi there\n"
            + f"AUDIO {len(wav_data)}\n".encode()
            + wav_data[:12]
        )
        messages.extend(parser.feed(wav_data[12:]))

        self.assertEqual(messages, [
            ("text", "Recognized: Hello"),
            ("text", "Answer: Hi there"),
            ("audio_start", str(len(wav_data))),
            ("audio_progress", f"12/{len(wav_data)}"),
            ("audio_progress", f"{len(wav_data)}/{len(wav_data)}"),
            ("audio", wav_data),
        ])

    def test_stream_response_parser_rejects_oversized_audio(self):
        parser = mac_voice_client.StreamResponseParser()

        with self.assertRaisesRegex(ConnectionError, "audio frame size"):
            parser.feed(
                f"AUDIO {mac_voice_client.MAX_STREAM_AUDIO_BYTES + 1}\n".encode()
            )

    def test_microphone_frame_buffer_keeps_recent_audio_when_full(self):
        microphone = mac_voice_client.MicrophoneFrameBuffer(max_frames=2)
        microphone.callback(b"frame-1", 1, None, None)
        microphone.callback(b"frame-2", 1, None, None)
        microphone.callback(b"frame-3", 1, None, None)

        self.assertTrue(microphone.frames_dropped.is_set())
        self.assertEqual(microphone.get(timeout=0), b"frame-2")
        self.assertEqual(microphone.get(timeout=0), b"frame-3")

    def test_microphone_frame_buffer_reports_portaudio_overflow(self):
        microphone = mac_voice_client.MicrophoneFrameBuffer(max_frames=2)
        microphone.callback(
            b"frame", 1, None, MagicMock(input_overflow=True)
        )

        self.assertTrue(microphone.input_overflowed.is_set())
        self.assertEqual(microphone.get(timeout=0), b"frame")

    def test_microphone_frame_buffer_discards_stale_frames(self):
        microphone = mac_voice_client.MicrophoneFrameBuffer(max_frames=2)
        microphone.callback(b"stale-1", 1, None, None)
        microphone.callback(b"stale-2", 1, None, None)

        microphone.discard_pending()

        self.assertTrue(microphone.frames.empty())

    @patch.object(mac_voice_client, "send_recording")
    def test_record_command_uses_custom_duration(self, send_recording):
        keep_running = mac_voice_client.handle_input(
            "/record 7", "http://esp32.local", 30.0, 5.0
        )

        self.assertTrue(keep_running)
        send_recording.assert_called_once_with(
            "http://esp32.local", 7.0, 30.0, "esp32"
        )

    @patch.object(mac_voice_client, "send_recording")
    def test_record_command_uses_macos_speech_when_selected(self, send_recording):
        speech_settings = mac_voice_client.SpeechSettings()
        speech_settings.backend = "macos"

        keep_running = mac_voice_client.handle_input(
            "/record 7", "http://esp32.local", 30.0, 5.0, speech_settings
        )

        self.assertTrue(keep_running)
        send_recording.assert_called_once_with(
            "http://esp32.local", 7.0, 30.0, "macos"
        )

    @patch.object(mac_voice_client, "send_speech")
    def test_speak_command_sends_text_to_synthesizer(self, send_speech):
        keep_running = mac_voice_client.handle_input(
            "/speak Hello there", "http://esp32.local", 30.0, 5.0
        )

        self.assertTrue(keep_running)
        send_speech.assert_called_once_with(
            "http://esp32.local", "Hello there", 30.0
        )

    @patch.object(mac_voice_client, "stream_audio")
    def test_stream_command_starts_continuous_audio(self, stream_audio):
        keep_running = mac_voice_client.handle_input(
            "/begin", "http://esp32.local", 30.0, 5.0
        )

        self.assertTrue(keep_running)
        stream_audio.assert_called_once_with("http://esp32.local", 30.0, "esp32")

    @patch.object(mac_voice_client, "stream_audio")
    def test_mac_command_selects_offline_speech_for_stream(self, stream_audio):
        speech_settings = mac_voice_client.SpeechSettings()
        with patch("builtins.print"):
            mac_voice_client.handle_input(
                "/mac", "http://esp32.local", 30.0, 5.0, speech_settings
            )
        mac_voice_client.handle_input(
            "/begin", "http://esp32.local", 30.0, 5.0, speech_settings
        )

        self.assertEqual(speech_settings.backend, "macos")
        stream_audio.assert_called_once_with("http://esp32.local", 30.0, "macos")

    @patch.object(mac_voice_client, "stream_audio")
    def test_esp_command_restores_esp_speech(self, stream_audio):
        speech_settings = mac_voice_client.SpeechSettings()
        speech_settings.backend = "macos"
        with patch("builtins.print"):
            mac_voice_client.handle_input(
                "/esp", "http://esp32.local", 30.0, 5.0, speech_settings
            )
        mac_voice_client.handle_input(
            "/begin", "http://esp32.local", 30.0, 5.0, speech_settings
        )

        self.assertEqual(speech_settings.backend, "esp32")
        stream_audio.assert_called_once_with("http://esp32.local", 30.0, "esp32")

    @patch.object(mac_voice_client, "stream_audio")
    def test_stream_command_remains_an_alias_for_begin(self, stream_audio):
        keep_running = mac_voice_client.handle_input(
            "/stream", "http://esp32.local", 30.0, 5.0
        )

        self.assertTrue(keep_running)
        stream_audio.assert_called_once_with("http://esp32.local", 30.0, "esp32")

    def test_stream_messages_explain_conversation_states(self):
        with patch("builtins.print") as print_output:
            mac_voice_client.display_stream_message("EVENT WAKE_REQUIRED")
            mac_voice_client.display_stream_message("EVENT WAKE")

        self.assertEqual(print_output.call_count, 2)
        self.assertIn("Hi ESP", print_output.call_args_list[0].args[0])
        self.assertIn("Wake word heard", print_output.call_args_list[1].args[0])

    def test_exit_command_does_not_record(self):
        with patch.object(mac_voice_client, "send_recording") as send_recording:
            keep_running = mac_voice_client.handle_input(
                "/exit", "http://esp32.local", 30.0, 5.0
            )

        self.assertFalse(keep_running)
        send_recording.assert_not_called()


if __name__ == "__main__":
    unittest.main()
