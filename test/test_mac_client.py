import importlib.util
import pathlib
import unittest
from unittest.mock import Mock, patch


MODULE_PATH = pathlib.Path(__file__).parents[1] / "mac_client.py"
SPEC = importlib.util.spec_from_file_location("mac_client", MODULE_PATH)
mac_client = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mac_client)


class MacClientCommandTests(unittest.TestCase):
    @patch.object(mac_client, "send_question")
    def test_plain_exit_is_sent_as_a_question(self, send_question):
        should_continue = mac_client.handle_input(
            "exit", "http://esp32.local", 3.0
        )

        self.assertTrue(should_continue)
        send_question.assert_called_once_with("http://esp32.local", "exit", 3.0)

    def test_slash_exit_stops_without_sending_a_question(self):
        with patch.object(mac_client, "send_question") as send_question:
            should_continue = mac_client.handle_input(
                "/exit", "http://esp32.local", 3.0
            )

        self.assertFalse(should_continue)
        send_question.assert_not_called()

    @patch.object(mac_client, "check_health", return_value="ready")
    def test_health_command_checks_the_device(self, check_health):
        with patch("builtins.print") as print_output:
            should_continue = mac_client.handle_input(
                "/health", "http://esp32.local", 3.0
            )

        self.assertTrue(should_continue)
        check_health.assert_called_once_with("http://esp32.local", 3.0)
        print_output.assert_called_once_with("ESP32> ready")

    @patch.object(mac_client, "send_question")
    def test_ask_command_sends_its_argument(self, send_question):
        mac_client.handle_input(
            "/ask how do you answer question", "http://esp32.local", 3.0
        )

        send_question.assert_called_once_with(
            "http://esp32.local", "how do you answer question", 3.0
        )

    @patch.object(mac_client, "send_question")
    def test_unknown_slash_command_is_not_sent_as_question(self, send_question):
        with patch("builtins.print"):
            should_continue = mac_client.handle_input(
                "/poweroff", "http://esp32.local", 3.0
            )

        self.assertTrue(should_continue)
        send_question.assert_not_called()


if __name__ == "__main__":
    unittest.main()
