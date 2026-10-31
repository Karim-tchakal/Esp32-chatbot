#include <Arduino.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>

#include <cstring>
#include <string>

#include "faq_search.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define CHATBOT_WIFI_SSID ""
#define CHATBOT_WIFI_PASSWORD ""
#endif

namespace {

constexpr char kFaqPath[] = "/faq.txt";
constexpr size_t kInputCapacity = 256;
constexpr size_t kFaqLineCapacity = 768;
constexpr size_t kMaximumHttpBodyLength = 255;
constexpr int kMinimumMatchScore = 55;
constexpr int kAmbiguousScoreMargin = 8;
constexpr uint32_t kWifiRetryIntervalMs = 10000;
constexpr uint32_t kWifiConnectTimeoutMs = 20000;

char inputBuffer[kInputCapacity];
size_t inputLength = 0;
bool inputOverflow = false;
bool filesystemReady = false;
bool httpServerReady = false;
uint32_t lastWifiAttempt = 0;
WebServer server(80);

struct Match {
  String question;
  String answer;
  int score = 0;
};

enum class AnswerStatus {
  Found,
  Unknown,
  Ambiguous,
  StorageUnavailable,
  DatabaseInvalid,
};

bool readFaqLine(File& file, char* buffer, size_t capacity, bool& overflow) {
  size_t length = 0;
  overflow = false;

  while (file.available()) {
    const int next = file.read();
    if (next < 0 || next == '\n') {
      break;
    }

    if (next == '\r') {
      continue;
    }

    if (length + 1 < capacity) {
      buffer[length++] = static_cast<char>(next);
    } else {
      overflow = true;
    }
  }

  buffer[length] = '\0';
  return length > 0 || overflow;
}

bool splitFaqLine(char* line, char*& question, char*& answer,
                  char*& keywords) {
  char* firstSeparator = std::strchr(line, '|');
  if (firstSeparator == nullptr) {
    return false;
  }
  *firstSeparator = '\0';

  char* secondSeparator = std::strchr(firstSeparator + 1, '|');
  if (secondSeparator == nullptr) {
    return false;
  }
  *secondSeparator = '\0';

  char* thirdSeparator = std::strchr(secondSeparator + 1, '|');
  if (thirdSeparator == nullptr || std::strchr(thirdSeparator + 1, '|')) {
    return false;
  }
  *thirdSeparator = '\0';

  question = firstSeparator + 1;
  answer = secondSeparator + 1;
  keywords = thirdSeparator + 1;
  return question[0] != '\0' && answer[0] != '\0';
}

AnswerStatus findAnswer(const char* question, String& answer,
                        String& diagnostic) {
  if (!filesystemReady) {
    diagnostic = "FAQ storage is unavailable.";
    return AnswerStatus::StorageUnavailable;
  }

  File database = LittleFS.open(kFaqPath, FILE_READ);
  if (!database) {
    diagnostic = "Could not open /faq.txt. Upload the FAQ filesystem.";
    return AnswerStatus::StorageUnavailable;
  }

  Match best;
  Match secondBest;
  char line[kFaqLineCapacity];
  unsigned int records = 0;
  unsigned int skippedLines = 0;

  while (database.available()) {
    bool lineOverflow = false;
    if (!readFaqLine(database, line, sizeof(line), lineOverflow)) {
      continue;
    }
    if (lineOverflow) {
      ++skippedLines;
      continue;
    }
    if (line[0] == '\0' || line[0] == '#') {
      continue;
    }

    char* faqQuestion = nullptr;
    char* faqAnswer = nullptr;
    char* keywords = nullptr;
    if (!splitFaqLine(line, faqQuestion, faqAnswer, keywords)) {
      ++skippedLines;
      continue;
    }
    ++records;

    const std::string query(question);
    const int questionScore = faq::similarity(query, faqQuestion);
    const int keywordScore = faq::similarity(query, keywords);
    const int score =
        questionScore > keywordScore ? questionScore : keywordScore;
    if (score <= 0) {
      continue;
    }

    if (String(faqAnswer) == best.answer) {
      if (score > best.score) {
        best.question = faqQuestion;
        best.score = score;
      }
      continue;
    }

    if (score > best.score) {
      secondBest = best;
      best.question = faqQuestion;
      best.answer = faqAnswer;
      best.score = score;
    } else if (String(faqAnswer) == secondBest.answer) {
      if (score > secondBest.score) {
        secondBest.question = faqQuestion;
        secondBest.score = score;
      }
    } else if (score > secondBest.score) {
      secondBest.question = faqQuestion;
      secondBest.answer = faqAnswer;
      secondBest.score = score;
    }
  }
  database.close();

  if (skippedLines > 0) {
    diagnostic = String("Skipped ") + String(skippedLines) +
                 " malformed or overlong FAQ line(s).";
  }
  if (records == 0) {
    diagnostic += diagnostic.length() > 0 ? " " : "";
    diagnostic += "/faq.txt contains no usable FAQ entries.";
    return AnswerStatus::DatabaseInvalid;
  }
  if (best.score < kMinimumMatchScore) {
    answer = "I don't know that one yet. Please add it to faq.txt.";
    return AnswerStatus::Unknown;
  }
  if (secondBest.score >= best.score - kAmbiguousScoreMargin) {
    answer = String("I found a few close matches. Please rephrase your question. "
                    "Possible matches: ") +
             best.question + "; " + secondBest.question;
    return AnswerStatus::Ambiguous;
  }

  answer = best.answer;
  return AnswerStatus::Found;
}

void printAnswer(const char* question) {
  String answer;
  String diagnostic;
  const AnswerStatus status = findAnswer(question, answer, diagnostic);

  if (diagnostic.length() > 0) {
    Serial.printf("WARNING: %s\n", diagnostic.c_str());
  }
  if (status == AnswerStatus::StorageUnavailable ||
      status == AnswerStatus::DatabaseInvalid) {
    Serial.printf("ERROR: %s\n", answer.length() > 0 ? answer.c_str()
                                                    : diagnostic.c_str());
    return;
  }
  Serial.printf("ANSWER: %s\n", answer.c_str());
}

void handleHealth() {
  if (!filesystemReady || !LittleFS.exists(kFaqPath)) {
    server.send(503, "text/plain; charset=utf-8",
                "ESP32 is online, but the FAQ file is unavailable.\n");
    return;
  }
  server.send(200, "text/plain; charset=utf-8",
              "ESP32 offline FAQ assistant is ready.\n");
}

void handleAsk() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain; charset=utf-8",
                "Send the question as a UTF-8 text/plain request body.\n");
    return;
  }

  String question = server.arg("plain");
  question.trim();
  if (question.length() == 0) {
    server.send(400, "text/plain; charset=utf-8", "Question cannot be empty.\n");
    return;
  }
  if (question.length() > kMaximumHttpBodyLength) {
    server.send(413, "text/plain; charset=utf-8",
                "Question is too long; maximum is 255 bytes.\n");
    return;
  }

  String answer;
  String diagnostic;
  const AnswerStatus status =
      findAnswer(question.c_str(), answer, diagnostic);
  if (diagnostic.length() > 0) {
    Serial.printf("WARNING: %s\n", diagnostic.c_str());
  }

  switch (status) {
    case AnswerStatus::Found:
    case AnswerStatus::Unknown:
    case AnswerStatus::Ambiguous:
      server.send(200, "text/plain; charset=utf-8", answer);
      Serial.printf("Wi-Fi question: %s\nANSWER: %s\n", question.c_str(),
                    answer.c_str());
      break;
    case AnswerStatus::StorageUnavailable:
      server.send(503, "text/plain; charset=utf-8",
                  diagnostic.length() > 0 ? diagnostic
                                          : "FAQ storage is unavailable.\n");
      break;
    case AnswerStatus::DatabaseInvalid:
      server.send(500, "text/plain; charset=utf-8",
                  diagnostic.length() > 0
                      ? diagnostic
                      : "/faq.txt contains no usable FAQ entries.\n");
      break;
  }
}

void startHttpServer() {
  server.on("/", HTTP_GET, handleHealth);
  server.on("/health", HTTP_GET, handleHealth);
  server.on("/ask", HTTP_POST, handleAsk);
  server.onNotFound([]() {
    server.send(404, "text/plain; charset=utf-8",
                "Not found. Use GET /health or POST /ask.\n");
  });
  server.begin();
  httpServerReady = true;
  Serial.printf("FAQ API listening at http://%s/ask\n",
                WiFi.localIP().toString().c_str());
}

void connectToWifi() {
  if (WiFi.status() == WL_CONNECTED || strlen(CHATBOT_WIFI_SSID) == 0) {
    return;
  }

  const uint32_t now = millis();
  if (lastWifiAttempt != 0 &&
      static_cast<uint32_t>(now - lastWifiAttempt) < kWifiRetryIntervalMs) {
    return;
  }
  lastWifiAttempt = now;

  Serial.printf("Connecting to Wi-Fi network \"%s\"...\n", CHATBOT_WIFI_SSID);
  WiFi.begin(CHATBOT_WIFI_SSID, CHATBOT_WIFI_PASSWORD);
  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         static_cast<uint32_t>(millis() - startedAt) <
             kWifiConnectTimeoutMs) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Wi-Fi connected. ESP32 IP: %s\n",
                  WiFi.localIP().toString().c_str());
    if (!httpServerReady) {
      startHttpServer();
    }
  } else {
    Serial.println(
        "ERROR: Wi-Fi connection timed out. Check the local Wi-Fi config.");
  }
}

void handleInput() {
  inputBuffer[inputLength] = '\0';

  if (inputOverflow) {
    Serial.println("ERROR: question is too long (maximum 255 bytes).");
  } else {
    String question(inputBuffer);
    question.trim();
    if (question == "!help") {
      Serial.println("Type an English question and press Enter.");
      Serial.println("Edit data/faq.txt to add questions, answers, and search terms.");
      Serial.println("This prototype accepts text over USB serial or POST /ask.");
    } else if (question.length() > 0) {
      printAnswer(question.c_str());
    }
  }

  inputLength = 0;
  inputOverflow = false;
  Serial.print("\nquestion> ");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println("\nOffline ESP32 FAQ assistant");
  Serial.printf("PSRAM: %s\n", psramFound() ? "available" : "not detected");

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("esp32-faq");

  filesystemReady = LittleFS.begin(false, "/littlefs", 10, "spiffs");
  if (!filesystemReady) {
    Serial.println("LittleFS mount failed; retrying after formatting its partition.");
    filesystemReady = LittleFS.begin(true, "/littlefs", 10, "spiffs");
    if (filesystemReady) {
      Serial.println(
          "LittleFS recovered by formatting. Upload the filesystem image to "
          "restore data/faq.txt.");
    } else {
      Serial.println(
          "ERROR: LittleFS could not be mounted or formatted. Check "
          "partitions.csv and upload the filesystem image.");
    }
  }

  if (strlen(CHATBOT_WIFI_SSID) == 0) {
    Serial.println(
        "Wi-Fi is not configured. Copy wifi_secrets.h.example to "
        "include/wifi_secrets.h and set your hotspot credentials.");
  } else {
    connectToWifi();
  }

  Serial.println("Type a question and press Enter. Type !help for usage.");
  Serial.print("\nquestion> ");
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!httpServerReady) {
      startHttpServer();
    }
    server.handleClient();
  } else if (strlen(CHATBOT_WIFI_SSID) > 0) {
    connectToWifi();
  }

  while (Serial.available() > 0) {
    const char next = static_cast<char>(Serial.read());

    if (next == '\r') {
      continue;
    }
    if (next == '\n') {
      handleInput();
      continue;
    }
    if (inputOverflow) {
      continue;
    }
    if (next == '\0') {
      continue;
    }
    if (inputLength + 1 >= sizeof(inputBuffer)) {
      inputOverflow = true;
      continue;
    }
    inputBuffer[inputLength++] = next;
  }
}
