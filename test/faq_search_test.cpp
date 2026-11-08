#include <cassert>
#include <fstream>
#include <sstream>
#include <string>

#include "faq_search.h"

static int bestFaqMatch(const std::string& query, const std::string& id) {
  std::ifstream database("data/faq.txt");
  assert(database.good());

  int bestScore = 0;
  std::string bestId;
  std::string bestAnswer;
  int secondScore = 0;
  std::string secondAnswer;
  std::string line;

  while (std::getline(database, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    std::istringstream fields(line);
    std::string entryId;
    std::string question;
    std::string answer;
    std::string keywords;
    if (!std::getline(fields, entryId, '|') ||
        !std::getline(fields, question, '|') ||
        !std::getline(fields, answer, '|') ||
        !std::getline(fields, keywords)) {
      continue;
    }

    const int questionScore = faq::similarity(query, question);
    const int keywordScore = faq::similarity(query, keywords);
    const int score =
        questionScore > keywordScore ? questionScore : keywordScore;
    if (answer == bestAnswer) {
      bestScore = score > bestScore ? score : bestScore;
    } else if (score > bestScore) {
      secondScore = bestScore;
      secondAnswer = bestAnswer;
      bestScore = score;
      bestAnswer = answer;
      bestId = entryId;
    } else if (answer == secondAnswer) {
      secondScore = score > secondScore ? score : secondScore;
    } else if (score > secondScore) {
      secondScore = score;
      secondAnswer = answer;
    }
  }

  if (bestId != id || bestScore < 55 || secondScore >= bestScore - 8) {
    return 0;
  }
  return bestScore;
}

int main() {
  const std::string timeQuestion =
      "What time is it? The current local time";
  const std::string weatherQuestion =
      "What is the weather forecast? Weather rain temperature";
  const std::string answerQuestion =
      "How do you answer questions? database local search faq matching";
  const std::string updateQuestion =
      "How do I add an answer? add edit update faq file upload";

  assert(faq::similarity("what time is it", timeQuestion) >
         faq::similarity("what time is it", weatherQuestion));
  assert(faq::similarity("what can you do", "What can you do?") == 100);
  assert(faq::similarity("what can u do", "What can you do?") == 100);
  assert(faq::similarity("what can you do", timeQuestion) == 0);
  assert(faq::similarity("tell me the time", timeQuestion) >= 55);
  assert(faq::similarity("what is the weathr", weatherQuestion) >= 55);
  assert(faq::similarity("how do I bake bread", timeQuestion) < 55);
  assert(faq::similarity("what is usability", "What is usability?") == 100);
  assert(faq::similarity("what is usability",
                         "What is the System Usability Scale?") == 0);
  assert(faq::similarity("define prototype", "What is a prototype?") == 100);
  assert(faq::similarity("describe prototype", "What is a prototype?") == 100);
  assert(faq::similarity("explain usability testing",
                         "What is usability testing?") == 100);
  assert(faq::similarity("meaning of persona", "What is a user persona?") >=
         80);
  assert(faq::similarity("what does WCAG mean", "What does WCAG mean?") ==
         100);
  assert(faq::similarity("define prototype",
                         "What is a low fidelity prototype?") < 100);
  assert(faq::similarity("the is and what", timeQuestion) == 0);
  assert(faq::similarity("weather forecast", weatherQuestion) >= 55);
  assert(faq::similarity("how do you answer question", answerQuestion) >
         faq::similarity("how do you answer question", updateQuestion) + 8);
  assert(faq::similarity("exit", updateQuestion) == 0);
  assert(faq::similarity("what aare you", "What are you?") >= 90);
  assert(faq::similarity("hi", "What hardware are you running on?") == 0);
  assert(faq::similarity("hi", "Hello! hello hi hey greetings") == 100);

  assert(bestFaqMatch("hi", "hello") > 0);
  assert(bestFaqMatch("nice", "nice") > 0);
  assert(bestFaqMatch("how are you", "how_are_you") > 0);
  assert(bestFaqMatch("what aare you", "identity") > 0);
  assert(bestFaqMatch("what can u do", "capabilities") > 0);
  assert(bestFaqMatch("hmm there is a problem", "problem") > 0);
  assert(bestFaqMatch("what is psram", "psram") > 0);
  assert(bestFaqMatch("how do you answer question", "database") > 0);
  assert(bestFaqMatch("what is usability", "usability_01") > 0);
  assert(bestFaqMatch("define prototype", "prototype_01") > 0);
  assert(bestFaqMatch("define usability testing", "usability_testing_01") > 0);
  assert(bestFaqMatch("can you explain fitts's law", "fitts_law_01") > 0);
  assert(bestFaqMatch("what does wcag mean", "wcag_03") > 0);
  assert(bestFaqMatch("what is a microcontroller", "microcontroller_01") > 0);
}
