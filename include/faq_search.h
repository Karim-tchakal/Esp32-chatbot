#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace faq {

namespace detail {

inline std::string normalize(const std::string& text) {
  std::string result;
  result.reserve(text.size());
  bool previousWasSpace = true;

  for (const unsigned char character : text) {
    if (std::isalnum(character)) {
      result.push_back(static_cast<char>(std::tolower(character)));
      previousWasSpace = false;
    } else if (!previousWasSpace) {
      result.push_back(' ');
      previousWasSpace = true;
    }
  }

  if (!result.empty() && result.back() == ' ') {
    result.pop_back();
  }

  std::string expanded;
  size_t start = 0;
  while (start < result.size()) {
    const size_t end = result.find(' ', start);
    const size_t length =
        (end == std::string::npos ? result.size() : end) - start;
    const std::string token = result.substr(start, length);
    if (!expanded.empty()) {
      expanded.push_back(' ');
    }
    expanded += token == "u" ? "you" : token;
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return expanded;
}

inline bool isStopWord(const std::string& token) {
  static constexpr std::array<const char*, 29> stopWords = {
      "a",    "an",   "and",  "are", "as",   "at",   "be",   "been",
      "can",  "do",   "does", "for", "from", "how",  "i",    "in",
      "is",   "it",   "me",   "of",  "on",   "or",   "please", "the",
      "this", "to",   "was",  "what", "you"};
  return std::any_of(stopWords.begin(), stopWords.end(),
                     [&token](const char* stopWord) {
                       return token == stopWord;
                     });
}

inline std::vector<std::string> tokens(const std::string& text) {
  std::vector<std::string> result;
  const std::string normalized = normalize(text);
  size_t start = 0;

  while (start < normalized.size()) {
    const size_t end = normalized.find(' ', start);
    const size_t length =
        (end == std::string::npos ? normalized.size() : end) - start;
    std::string token = normalized.substr(start, length);
    if (token.size() > 1 && !isStopWord(token) &&
        std::find(result.begin(), result.end(), token) == result.end()) {
      result.push_back(std::move(token));
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return result;
}

inline std::vector<std::string> phraseTokens(const std::string& text) {
  std::vector<std::string> result;
  const std::string normalized = normalize(text);
  size_t start = 0;

  while (start < normalized.size()) {
    const size_t end = normalized.find(' ', start);
    const size_t length =
        (end == std::string::npos ? normalized.size() : end) - start;
    result.push_back(normalized.substr(start, length));
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return result;
}

inline bool oneEditApart(const std::string& left, const std::string& right) {
  if (left.size() < 3 || right.size() < 3 ||
      left.size() > right.size() + 1 || right.size() > left.size() + 1) {
    return false;
  }

  size_t leftIndex = 0;
  size_t rightIndex = 0;
  unsigned int edits = 0;

  while (leftIndex < left.size() && rightIndex < right.size()) {
    if (left[leftIndex] == right[rightIndex]) {
      ++leftIndex;
      ++rightIndex;
      continue;
    }

    if (++edits > 1) {
      return false;
    }
    if (left.size() > right.size()) {
      ++leftIndex;
    } else if (right.size() > left.size()) {
      ++rightIndex;
    } else {
      ++leftIndex;
      ++rightIndex;
    }
  }

  edits += leftIndex < left.size() || rightIndex < right.size() ? 1 : 0;
  return edits == 1;
}

inline bool phraseWindowOneEditApart(
    const std::vector<std::string>& queryTokens, size_t queryStart,
    size_t queryLength, const std::vector<std::string>& candidateTokens,
    size_t candidateStart, size_t candidateLength) {
  if (queryLength >= 14 || candidateLength >= 14) {
    return false;
  }

  std::array<size_t, 15> previous{};
  std::array<size_t, 15> current{};
  for (size_t column = 0; column <= candidateLength; ++column) {
    previous[column] = column;
  }

  for (size_t row = 1; row <= queryLength; ++row) {
    current[0] = row;
    size_t minimum = current[0];
    for (size_t column = 1; column <= candidateLength; ++column) {
      const std::string& queryToken = queryTokens[queryStart + row - 1];
      const std::string& candidateToken =
          candidateTokens[candidateStart + column - 1];
      size_t substitutionCost = 2;
      if (queryToken == candidateToken) {
        substitutionCost = 0;
      } else if (oneEditApart(queryToken, candidateToken)) {
        substitutionCost = 1;
      }

      current[column] =
          std::min({previous[column] + 1, current[column - 1] + 1,
                    previous[column - 1] + substitutionCost});
      minimum = std::min(minimum, current[column]);
    }
    if (minimum > 1) {
      return false;
    }
    previous = current;
  }

  return previous[candidateLength] <= 1;
}

inline bool phraseHasOneTypo(const std::string& query,
                             const std::string& candidate) {
  const std::vector<std::string> queryTokens = phraseTokens(query);
  const std::vector<std::string> candidateTokens = phraseTokens(candidate);
  if (queryTokens.size() < 3 || queryTokens.size() >= 14) {
    return false;
  }

  for (size_t start = 0; start < candidateTokens.size(); ++start) {
    const size_t minimumLength = queryTokens.size() > 0
                                     ? queryTokens.size() - 1
                                     : queryTokens.size();
    const size_t maximumLength = queryTokens.size() + 1;
    for (size_t length = minimumLength;
         length <= maximumLength && start + length <= candidateTokens.size();
         ++length) {
      if (phraseWindowOneEditApart(queryTokens, 0, queryTokens.size(),
                                   candidateTokens, start, length)) {
        return true;
      }
    }
  }
  return false;
}

inline int tokenMatchScore(const std::string& queryToken,
                           const std::vector<std::string>& candidateTokens) {
  for (const std::string& candidateToken : candidateTokens) {
    if (queryToken == candidateToken) {
      return 4;
    }
  }
  for (const std::string& candidateToken : candidateTokens) {
    if (oneEditApart(queryToken, candidateToken)) {
      return 2;
    }
  }
  return 0;
}

inline bool hasExactTokenMatch(const std::string& queryToken,
                               const std::vector<std::string>& tokens) {
  return std::find(tokens.begin(), tokens.end(), queryToken) != tokens.end();
}

inline bool containsWholePhrase(const std::string& candidate,
                                const std::string& phrase) {
  if (phrase.empty()) {
    return false;
  }

  size_t position = candidate.find(phrase);
  while (position != std::string::npos) {
    const size_t end = position + phrase.size();
    const bool startsAtWordBoundary =
        position == 0 || candidate[position - 1] == ' ';
    const bool endsAtWordBoundary =
        end == candidate.size() || candidate[end] == ' ';
    if (startsAtWordBoundary && endsAtWordBoundary) {
      return true;
    }
    position = candidate.find(phrase, position + 1);
  }
  return false;
}

}  // namespace detail

inline int similarity(const std::string& query, const std::string& candidate) {
  const std::string normalizedQuery = detail::normalize(query);
  const std::string normalizedCandidate = detail::normalize(candidate);
  const std::vector<std::string> queryPhraseTokens =
      detail::phraseTokens(query);
  const std::vector<std::string> queryTokens = detail::tokens(query);
  bool exactPhraseMatch =
      detail::containsWholePhrase(normalizedCandidate, normalizedQuery);
  if (exactPhraseMatch && queryTokens.size() == 1 &&
      queryPhraseTokens.size() > 1 && normalizedCandidate != normalizedQuery) {
    const size_t phraseStart = normalizedCandidate.find(normalizedQuery);
    const size_t nextWordStart = phraseStart + normalizedQuery.size() + 1;
    if (nextWordStart < normalizedCandidate.size()) {
      const size_t nextWordEnd = normalizedCandidate.find(' ', nextWordStart);
      const std::string nextWord = normalizedCandidate.substr(
          nextWordStart, nextWordEnd == std::string::npos
                             ? std::string::npos
                             : nextWordEnd - nextWordStart);
      exactPhraseMatch = detail::isStopWord(nextWord);
    }
  }
  if (exactPhraseMatch) {
    return 100;
  }
  if (queryTokens.size() == 1 && queryPhraseTokens.size() > 1 &&
      detail::hasExactTokenMatch(
          queryTokens.front(), detail::tokens(normalizedCandidate))) {
    return 0;
  }
  if (detail::phraseHasOneTypo(query, candidate)) {
    return 90;
  }

  if (queryTokens.empty()) {
    return 0;
  }

  const std::vector<std::string> candidateTokens = detail::tokens(candidate);
  if (candidateTokens.empty()) {
    return 0;
  }

  int total = 0;
  int matched = 0;
  int exactMatches = 0;
  for (const std::string& queryToken : queryTokens) {
    const int score = detail::tokenMatchScore(queryToken, candidateTokens);
    total += score;
    matched += score > 0 ? 1 : 0;
    exactMatches += detail::hasExactTokenMatch(queryToken, candidateTokens) ? 1
                                                                           : 0;
  }

  if (normalizedQuery.find(' ') == std::string::npos && exactMatches == 0) {
    return 0;
  }

  int result = total * 100 / static_cast<int>(queryTokens.size() * 4);
  result += matched * 10 / static_cast<int>(queryTokens.size());

  if (queryPhraseTokens.size() >= 4) {
    result = std::min(result, 80);
  }

  return std::min(result, 100);
}

}  // namespace faq
