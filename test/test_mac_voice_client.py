import io
import importlib.util
import pathlib
import sys
import unittest
import wave
from unittest.mock import MagicMock, patch


MODULE_PATH = pathlib.Path(__file__).parents[1] / "mac_voice_client.py"
SPEC = importlib.util.spec_from_file_location("mac_voice_client", MODULE_PATH)
mac_voice_client = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mac_voice_client)


class MacVoiceClientTests(unittest.TestCase):
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
        sounddevice = MagicMock()
        speaker = sounddevice.RawOutputStream.return_value.__enter__.return_value
        with patch.dict(sys.modules, {"sounddevice": sounddevice}):
            mac_voice_client.play_speech(wav_data)

        sounddevice.RawOutputStream.assert_called_once_with(
            samplerate=16000, channels=1, dtype="int16"
        )
        speaker.write.assert_called_once_with(b"\x01\x00\x02\x00")

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
            "http://esp32.local", 7.0, 30.0
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
        stream_audio.assert_called_once_with("http://esp32.local", 30.0)

    @patch.object(mac_voice_client, "stream_audio")
    def test_stream_command_remains_an_alias_for_begin(self, stream_audio):
        keep_running = mac_voice_client.handle_input(
            "/stream", "http://esp32.local", 30.0, 5.0
        )

        self.assertTrue(keep_running)
        stream_audio.assert_called_once_with("http://esp32.local", 30.0)

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
