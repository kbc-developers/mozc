// Copyright 2010-2016, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "prediction/dictionary_predictor.h"

#include <algorithm>
#include <cctype>
#include <climits>   // INT_MAX
#include <cmath>
#include <list>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/flags.h"
#include "base/logging.h"
#include "base/number_util.h"
#include "base/util.h"
#include "composer/composer.h"
#include "converter/connector.h"
#include "converter/converter_interface.h"
#include "converter/immutable_converter_interface.h"
#include "converter/node_list_builder.h"
#include "converter/segmenter.h"
#include "converter/segments.h"
#include "dictionary/dictionary_interface.h"
#include "dictionary/pos_matcher.h"
#include "prediction/predictor_interface.h"
#include "prediction/suggestion_filter.h"
#include "prediction/zero_query_data.h"
#include "prediction/zero_query_list.h"
#include "prediction/zero_query_number_data.h"
#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "request/conversion_request.h"
#include "usage_stats/usage_stats.h"

// This flag is set by predictor.cc
// We can remove this after the ambiguity expansion feature get stable.
DEFINE_bool(enable_expansion_for_dictionary_predictor,
            false,
            "enable ambiguity expansion for dictionary_predictor");

DEFINE_bool(enable_mixed_conversion,
            false,
            "Enable mixed conversion feature");

DECLARE_bool(enable_typing_correction);

using mozc::dictionary::DictionaryInterface;
using mozc::dictionary::POSMatcher;
using mozc::dictionary::Token;
using mozc::usage_stats::UsageStats;

namespace mozc {

using commands::Request;

namespace {

// Used to emulate positive infinity for cost. This value is set for those
// candidates that are thought to be aggressive; thus we can eliminate such
// candidates from suggestion or prediction. Note that for this purpose we don't
// want to use INT_MAX because someone might further add penalty after cost is
// set to INT_MAX, which leads to overflow and consequently aggressive
// candidates would appear in the top results.
const int kInfinity = (2 << 20);

// Note that PREDICTION mode is much slower than SUGGESTION.
// Number of prediction calls should be minimized.
const size_t kSuggestionMaxResultsSize = 256;
const size_t kPredictionMaxResultsSize = 100000;

// Returns true if the |target| may be reduncant result.
bool MaybeRedundant(const string &reference, const string &target) {
  return Util::StartsWith(target, reference);
}

bool IsLatinInputMode(const ConversionRequest &request) {
  return (request.has_composer() &&
          (request.composer().GetInputMode() == transliteration::HALF_ASCII ||
           request.composer().GetInputMode() == transliteration::FULL_ASCII));
}

// Returns true if |segments| contains number history.
// Normalized number will be set to |number_key|
// Note:
//  Now this function supports arabic number candidates only and
//  we don't support kanji number candidates for now.
//  This is because We have several kanji number styles, for example,
//  "一二", "十二", "壱拾弐", etc for 12.
// TODO(toshiyuki): Define the spec and support Kanji.
bool GetNumberHistory(const Segments &segments, string *number_key) {
  DCHECK(number_key);
  const size_t history_size = segments.history_segments_size();
  if (history_size <= 0) {
    return false;
  }

  const Segment &last_segment = segments.history_segment(history_size - 1);
  DCHECK_GT(last_segment.candidates_size(), 0);
  const string &history_value = last_segment.candidate(0).value;
  if (!NumberUtil::IsArabicNumber(history_value)) {
    return false;
  }

  Util::FullWidthToHalfWidth(history_value, number_key);
  return true;
}

bool IsMixedConversionEnabled(const commands::Request& request) {
  return request.mixed_conversion() || FLAGS_enable_mixed_conversion;
}

bool IsTypingCorrectionEnabled(const ConversionRequest &request) {
  return request.config().use_typing_correction() ||
         FLAGS_enable_typing_correction;
}

struct ZeroQueryListCompare {
  bool operator()(const ZeroQueryList &lhs, const ZeroQueryList &rhs) const {
    return (strcmp(lhs.key, rhs.key) < 0);
  }
};
}  // namespace

class DictionaryPredictor::PredictiveLookupCallback :
      public DictionaryInterface::Callback {
 public:
  PredictiveLookupCallback(DictionaryPredictor::PredictionTypes types,
                           size_t limit, size_t original_key_len,
                           const set<string> *subsequent_chars,
                           bool is_zero_query,
                           vector<DictionaryPredictor::Result> *results)
      : penalty_(0), types_(types), limit_(limit),
        original_key_len_(original_key_len),
        subsequent_chars_(subsequent_chars),
        is_zero_query_(is_zero_query),
        results_(results) {}

  virtual ResultType OnKey(StringPiece key) {
    if (subsequent_chars_ == NULL) {
      return TRAVERSE_CONTINUE;
    }
    // If |subsequent_chars_| was provided, check if the substring of |key|
    // obtained by removing the original lookup key starts with a string in the
    // set.  For example, if original key is "he" and "hello" was found,
    // continue traversing only when one of "l", "ll", or "llo" is in
    // |subsequent_chars_|.
    // Implementation note: Although Util::StartsWith is called at most N times
    // where N = subsequent_chars_.size(), N is very small in practice, less
    // than 10.  Thus, this linear order algorithm is fast enough.
    // Theoretically, we can construct a trie of strings in |subsequent_chars_|
    // to get more performance but it's overkill here.
    // TODO(noriyukit): vector<string> would be better than set<string>.  To
    // this end, we need to fix Comopser as well.
    const StringPiece rest(key, original_key_len_);
    for (set<string>::const_iterator iter = subsequent_chars_->begin();
         iter != subsequent_chars_->end(); ++iter) {
      if (Util::StartsWith(rest, *iter)) {
        return TRAVERSE_CONTINUE;
      }
    }
    return TRAVERSE_NEXT_KEY;
  }

  virtual ResultType OnActualKey(StringPiece key, StringPiece actual_key,
                                 bool is_expanded) {
    penalty_ = is_expanded ? kKanaModifierInsensitivePenalty : 0;
    return TRAVERSE_CONTINUE;
  }

  virtual ResultType OnToken(StringPiece,  // key
                             StringPiece,  // actual_key
                             const Token &token) {
    results_->push_back(Result());
    results_->back().InitializeByTokenAndTypes(token, types_);
    results_->back().wcost += penalty_;
    if (is_zero_query_ && (types_ & SUFFIX)) {
      results_->back().SetSourceInfoForZeroQuery(ZERO_QUERY_SUFFIX);
    }
    if (results_->size() < limit_) {
      return TRAVERSE_CONTINUE;
    } else {
      return TRAVERSE_DONE;
    }
  }

 protected:
  int32 penalty_;
  const DictionaryPredictor::PredictionTypes types_;
  const size_t limit_;
  const size_t original_key_len_;
  const set<string> *subsequent_chars_;
  const bool is_zero_query_;
  vector<DictionaryPredictor::Result> *results_;

 private:
  DISALLOW_COPY_AND_ASSIGN(PredictiveLookupCallback);
};

class DictionaryPredictor::PredictiveBigramLookupCallback :
      public PredictiveLookupCallback {
 public:
  PredictiveBigramLookupCallback(DictionaryPredictor::PredictionTypes types,
                                 size_t limit, size_t original_key_len,
                                 const set<string> *subsequent_chars,
                                 StringPiece history_value,
                                 bool is_zero_query,
                                 vector<DictionaryPredictor::Result> *results)
      : PredictiveLookupCallback(types, limit, original_key_len,
                                 subsequent_chars, is_zero_query, results),
        history_value_(history_value) {}

  virtual ResultType OnToken(StringPiece key, StringPiece expanded_key,
                             const Token &token) {
    // Skip the token if its value doesn't start with the previous user input,
    // |history_value_|.
    if (!Util::StartsWith(token.value, history_value_) ||
        token.value.size() <= history_value_.size()) {
      return TRAVERSE_CONTINUE;
    }
    ResultType result_type =
        PredictiveLookupCallback::OnToken(key, expanded_key, token);
    if (is_zero_query_) {
      results_->back().SetSourceInfoForZeroQuery(
          ZERO_QUERY_BIGRAM);
    }
    return result_type;
  }

 private:
  StringPiece history_value_;

  DISALLOW_COPY_AND_ASSIGN(PredictiveBigramLookupCallback);
};

// Comparator for sorting prediction candidates.
// If we have words A and AB, for example "六本木" and "六本木ヒルズ",
// assume that cost(A) < cost(AB).
class DictionaryPredictor::ResultWCostLess :
      public binary_function<Result, Result, bool> {
 public:
  bool operator() (const DictionaryPredictor::Result &lhs,
                   const DictionaryPredictor::Result &rhs) const {
    return lhs.wcost < rhs.wcost;
  }
};

class DictionaryPredictor::ResultCostLess :
      public binary_function<Result, Result, bool> {
 public:
  bool operator() (const DictionaryPredictor::Result &lhs,
                   const DictionaryPredictor::Result &rhs) const {
    return lhs.cost > rhs.cost;
  }
};

DictionaryPredictor::DictionaryPredictor(
    const ConverterInterface *converter,
    const ImmutableConverterInterface *immutable_converter,
    const DictionaryInterface *dictionary,
    const DictionaryInterface *suffix_dictionary,
    const Connector *connector,
    const Segmenter *segmenter,
    const POSMatcher *pos_matcher,
    const SuggestionFilter *suggestion_filter)
    : converter_(converter),
      immutable_converter_(immutable_converter),
      dictionary_(dictionary),
      suffix_dictionary_(suffix_dictionary),
      connector_(connector),
      segmenter_(segmenter),
      suggestion_filter_(suggestion_filter),
      counter_suffix_word_id_(pos_matcher->GetCounterSuffixWordId()),
      predictor_name_("DictionaryPredictor") {}

DictionaryPredictor::~DictionaryPredictor() {}

void DictionaryPredictor::Finish(
    const ConversionRequest &request, Segments *segments) {
  if (segments->request_type() == Segments::REVERSE_CONVERSION) {
    // Do nothing for REVERSE_CONVERSION.
    return;
  }

  const Segment &segment = segments->conversion_segment(0);
  if (segment.candidates_size() < 1) {
    VLOG(2) << "candidates size < 1";
    return;
  }

  const Segment::Candidate &candidate = segment.candidate(0);
  if (segment.segment_type() != Segment::FIXED_VALUE) {
    VLOG(2) << "segment is not FIXED_VALUE" << candidate.value;
    return;
  }

  MaybeRecordUsageStats(candidate);
}

void DictionaryPredictor::MaybeRecordUsageStats(
    const Segment::Candidate &candidate) const {
  if (candidate.source_info &
      Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_NONE) {
    UsageStats::IncrementCount(
        "CommitDictionaryPredictorZeroQueryTypeNone");
  }

  if (candidate.source_info &
      Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_NUMBER_SUFFIX) {
    UsageStats::IncrementCount(
        "CommitDictionaryPredictorZeroQueryTypeNumberSuffix");
  }

  if (candidate.source_info &
      Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_EMOTICON) {
    UsageStats::IncrementCount(
        "CommitDictionaryPredictorZeroQueryTypeEmoticon");
  }

  if (candidate.source_info &
      Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_EMOJI) {
    UsageStats::IncrementCount(
        "CommitDictionaryPredictorZeroQueryTypeEmoji");
  }

  if (candidate.source_info &
      Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_BIGRAM) {
    UsageStats::IncrementCount(
        "CommitDictionaryPredictorZeroQueryTypeBigram");
  }

  if (candidate.source_info &
      Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_SUFFIX) {
    UsageStats::IncrementCount(
        "CommitDictionaryPredictorZeroQueryTypeSuffix");
  }
}

bool DictionaryPredictor::PredictForRequest(const ConversionRequest &request,
                                            Segments *segments) const {
  if (segments == NULL) {
    return false;
  }

  vector<Result> results;
  if (!AggregatePrediction(request, segments, &results)) {
    return false;
  }

  SetCost(request, *segments, &results);
  RemovePrediction(request, *segments, &results);

  return AddPredictionToCandidates(request, segments, &results);
}

bool DictionaryPredictor::AggregatePrediction(
    const ConversionRequest &request,
    Segments *segments,
    vector<Result> *results) const {
  DCHECK(segments);
  DCHECK(results);

  const PredictionTypes prediction_types =
      GetPredictionTypes(request, *segments);
  if (prediction_types == NO_PREDICTION) {
    return false;
  }

  if (segments->request_type() == Segments::PARTIAL_SUGGESTION ||
      segments->request_type() == Segments::PARTIAL_PREDICTION) {
    // This request type is used to get conversion before cursor during
    // composition mode. Thus it should return only the candidates whose key
    // exactly matches the query.
    // Therefore, we use only the realtime conversion result.
    AggregateRealtimeConversion(prediction_types, request, segments, results);
  } else {
    AggregateRealtimeConversion(prediction_types, request, segments, results);
    AggregateUnigramPrediction(prediction_types, request, *segments, results);
    AggregateBigramPrediction(prediction_types, request, *segments, results);
    AggregateSuffixPrediction(prediction_types, request, *segments, results);
    AggregateEnglishPrediction(prediction_types, request, *segments, results);
    AggregateTypeCorrectingPrediction(prediction_types, request, *segments,
                                      results);
  }

  if (results->empty()) {
    VLOG(2) << "|result| is empty";
    return false;
  } else {
    return true;
  }
}

void DictionaryPredictor::SetCost(const ConversionRequest &request,
                                  const Segments &segments,
                                  vector<Result> *results) const {
  DCHECK(results);

  if (IsMixedConversionEnabled(request.request())) {
    SetLMCost(segments, results);
  } else {
    SetPredictionCost(segments, results);
  }

  ApplyPenaltyForKeyExpansion(segments, results);
}

void DictionaryPredictor::RemovePrediction(const ConversionRequest &request,
                                           const Segments &segments,
                                           vector<Result> *results) const {
  DCHECK(results);

  if (!IsMixedConversionEnabled(request.request())) {
    // Currently, we don't have spelling correction feature on mobile,
    // so we don't run RemoveMissSpelledCandidates.
    const string &input_key = segments.conversion_segment(0).key();
    const size_t input_key_len = Util::CharsLen(input_key);
    RemoveMissSpelledCandidates(input_key_len, results);
  }
}

bool DictionaryPredictor::AddPredictionToCandidates(
    const ConversionRequest &request,
    Segments *segments, vector<Result> *results) const {
  DCHECK(segments);
  DCHECK(results);
  const bool mixed_conversion = IsMixedConversionEnabled(request.request());
  const string &input_key = segments->conversion_segment(0).key();
  const size_t input_key_len = Util::CharsLen(input_key);

  string history_key, history_value;
  GetHistoryKeyAndValue(*segments, &history_key, &history_value);

  // exact_bigram_key does not contain ambiguity expansion, because
  // this is used for exact matching for the key.
  const string exact_bigram_key = history_key + input_key;

  Segment *segment = segments->mutable_conversion_segment(0);
  DCHECK(segment);

  // Instead of sorting all the results, we construct a heap.
  // This is done in linear time and
  // we can pop as many results as we need efficiently.
  std::make_heap(results->begin(), results->end(), ResultCostLess());

  const size_t size = min(segments->max_prediction_candidates_size(),
                          results->size());

  int added = 0;
  set<string> seen;

  int added_suffix = 0;
  bool cursor_at_tail =
      request.has_composer() &&
      request.composer().GetCursor() == request.composer().GetLength();

  for (size_t i = 0; i < results->size(); ++i) {
    // Pop a result from a heap. Please pay attention not to use results->at(i).
    std::pop_heap(results->begin(), results->end() - i, ResultCostLess());
    const Result &result = results->at(results->size() - i - 1);

    if (added >= size || result.cost >= kInfinity) {
      break;
    }

    if (result.types == NO_PREDICTION) {
      continue;
    }

    // If mixed_conversion is true, we don't filter the results which have
    // the exact same key as the input.
    if (!(mixed_conversion && (result.key == input_key)) &&
        suggestion_filter_->IsBadSuggestion(result.value)) {
      continue;
    }

    // don't suggest exactly the same candidate as key.
    // if |mixed_conversion| is true, that's not the case.
    if (!mixed_conversion &&
        !(result.types & REALTIME) &&
        (((result.types & BIGRAM) &&
          exact_bigram_key == result.value) ||
         (!(result.types & BIGRAM) &&
          input_key == result.value))) {
      continue;
    }

    string key, value;
    if (result.types & BIGRAM) {
      // remove the prefix of history key and history value.
      key = result.key.substr(history_key.size(),
                              result.key.size() - history_key.size());
      value = result.value.substr(history_value.size(),
                                  result.value.size() - history_value.size());
    } else {
      key = result.key;
      value = result.value;
    }

    if (!seen.insert(value).second) {
      continue;
    }

    // User input: "おーすとり" (len = 5)
    // key/value:  "おーすとりら" "オーストラリア" (miss match pos = 4)
    if ((result.candidate_attributes &
         Segment::Candidate::SPELLING_CORRECTION) &&
        key != input_key &&
        input_key_len <= GetMissSpelledPosition(key, value) + 1) {
      continue;
    }

    if (result.types == SUFFIX && added_suffix++ >= 20) {
      // TODO(toshiyuki): Need refactoring for controlling suffix
      // prediction number after we will fix the appropriate number.
      continue;
    }

    Segment::Candidate *candidate = segment->push_back_candidate();
    DCHECK(candidate);

    candidate->Init();
    candidate->content_key = key;
    candidate->content_value = value;
    candidate->key = key;
    candidate->value = value;
    candidate->lid = result.lid;
    candidate->rid = result.rid;
    candidate->wcost = result.wcost;
    candidate->cost = result.cost;
    candidate->attributes = result.candidate_attributes;
    if ((!(candidate->attributes & Segment::Candidate::SPELLING_CORRECTION) &&
         IsLatinInputMode(request)) || (result.types & SUFFIX)) {
      candidate->attributes |= Segment::Candidate::NO_VARIANTS_EXPANSION;
      candidate->attributes |= Segment::Candidate::NO_EXTRA_DESCRIPTION;
    }
    if (candidate->attributes & Segment::Candidate::PARTIALLY_KEY_CONSUMED) {
      candidate->consumed_key_size = result.consumed_key_size;
      // There are two scenarios to reach here.
      // 1. Auto partial suggestion.
      //    e.g. composition わたしのなまえ| -> candidate 私の
      // 2. Partial suggestion.
      //    e.g. composition わたしの|なまえ -> candidate 私の
      // To distinguish auto partial suggestion from (non-auto) partial
      // suggestion, see the cursor position. If the cursor is at the tail
      // of the composition, this is auto partial suggestion.
      if (cursor_at_tail) {
        candidate->attributes |= Segment::Candidate::AUTO_PARTIAL_SUGGESTION;
      }
    }
    candidate->source_info = result.source_info;
    if (result.types & REALTIME) {
      candidate->inner_segment_boundary = result.inner_segment_boundary;
    }
    if (result.types & TYPING_CORRECTION) {
      candidate->attributes |= Segment::Candidate::TYPING_CORRECTION;
    }

    SetDescription(result.types, candidate->attributes,
                   &candidate->description);
#ifdef DEBUG
    SetDebugDescription(result.types, &candidate->description);
#endif  // DEBUG

    ++added;
  }
  return added > 0;
}

void DictionaryPredictor::SetDescription(PredictionTypes types,
                                         uint32 attributes,
                                         string *description) {
  if (types & TYPING_CORRECTION) {
    // "補正"
    Util::AppendStringWithDelimiter(
        " ",
        "\xE8\xA3\x9C\xE6\xAD\xA3",
        description);
  }
  if (attributes & Segment::Candidate::AUTO_PARTIAL_SUGGESTION) {
    // "部分"
    Util::AppendStringWithDelimiter(
        " ",
        "\xE9\x83\xA8\xE5\x88\x86",
        description);
  }
}

void DictionaryPredictor::SetDebugDescription(PredictionTypes types,
                                              string *description) {
  string debug_desc;
  if (types & UNIGRAM) {
    debug_desc.append(1, 'U');
  }
  if (types & BIGRAM) {
    debug_desc.append(1, 'B');
  }
  if (types & REALTIME_TOP) {
    debug_desc.append("R1");
  } else if (types & REALTIME) {
    debug_desc.append(1, 'R');
  }
  if (types & SUFFIX) {
    debug_desc.append(1, 'S');
  }
  if (types & ENGLISH) {
    debug_desc.append(1, 'E');
  }
  // Note that description for TYPING_CORRECTION is omitted
  // because it is appended by SetDescription.
  if (!debug_desc.empty()) {
    Util::AppendStringWithDelimiter(" ", debug_desc, description);
  }
}

// Returns cost for |result| when it's transitioned from |rid|.  Suffix penalty
// is also added for non-realtime results.
int DictionaryPredictor::GetLMCost(const Result &result, int rid) const {
  // Sometimes transition cost is too high and causes a bug like b/18112966.
  // For example, "接続詞 が" -> "始まる 動詞,五段活用,基本形" has very large cost
  // and "始まる" is demoted.  To prevent such cases, ImmutableConverter
  // computes transition from BOS/EOS too; see
  // ImmutableConverterImpl::MakeLatticeNodesForHistorySegments().
  // Here, taking the minimum of |cost1| and |cost2| has a similar effect.
  const int cost1 = connector_->GetTransitionCost(rid, result.lid);
  const int cost2 = connector_->GetTransitionCost(0, result.lid);
  int lm_cost = min(cost1, cost2) + result.wcost;
  if (!(result.types & REALTIME)) {
    // Relatime conversion already adds perfix/suffix penalties to the result.
    // Note that we don't add prefix penalty the role of "bunsetsu" is
    // ambigous on zero-query suggestion.
    lm_cost += segmenter_->GetSuffixPenalty(result.rid);
  }

  return lm_cost;
}

namespace {

class FindValueCallback : public DictionaryInterface::Callback {
 public:
  explicit FindValueCallback(StringPiece target_value)
      : target_value_(target_value), found_(false) {}

  virtual ResultType OnToken(StringPiece,  // key
                             StringPiece,  // actual_key
                             const Token &token) {
    if (token.value != target_value_) {
      return TRAVERSE_CONTINUE;
    }
    found_ = true;
    token_ = token;
    return TRAVERSE_DONE;
  }

  bool found() const {
    return found_;
  }

  const Token &token() const {
    return token_;
  }

 private:
  StringPiece target_value_;
  bool found_;
  Token token_;

  DISALLOW_COPY_AND_ASSIGN(FindValueCallback);
};

}  // namespace

void DictionaryPredictor::Result::InitializeByTokenAndTypes(
    const Token &token, PredictionTypes types) {
  SetTypesAndTokenAttributes(types, token.attributes);
  key = token.key;
  value = token.value;
  wcost = token.cost;
  lid = token.lid;
  rid = token.rid;
}

void DictionaryPredictor::Result::SetTypesAndTokenAttributes(
    PredictionTypes prediction_types, Token::AttributesBitfield token_attr) {
  types = prediction_types;
  candidate_attributes = 0;
  if (types & TYPING_CORRECTION) {
    candidate_attributes |= Segment::Candidate::TYPING_CORRECTION;
  }
  if (types & (REALTIME | REALTIME_TOP)) {
    candidate_attributes |= Segment::Candidate::REALTIME_CONVERSION;
  }
  if (token_attr & Token::SPELLING_CORRECTION) {
    candidate_attributes |= Segment::Candidate::SPELLING_CORRECTION;
  }
  if (token_attr & Token::USER_DICTIONARY) {
    candidate_attributes |= (Segment::Candidate::USER_DICTIONARY |
                             Segment::Candidate::NO_VARIANTS_EXPANSION);
  }
}

void DictionaryPredictor::Result::SetSourceInfoForZeroQuery(
    ZeroQueryType type) {
  switch (type) {
    case ZERO_QUERY_NONE:
      source_info |=
          Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_NONE;
      return;
    case ZERO_QUERY_NUMBER_SUFFIX:
      source_info |=
          Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_NUMBER_SUFFIX;
      return;
    case ZERO_QUERY_EMOTICON:
      source_info |=
          Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_EMOTICON;
      return;
    case ZERO_QUERY_EMOJI:
      source_info |=
          Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_EMOJI;
      return;
    case ZERO_QUERY_BIGRAM:
      source_info |=
          Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_BIGRAM;
      return;
    case ZERO_QUERY_SUFFIX:
      source_info |=
          Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_SUFFIX;
      return;
    default:
      LOG(ERROR) << "Should not come here";
      return;
  }
}

bool DictionaryPredictor::GetHistoryKeyAndValue(
    const Segments &segments, string *key, string *value) const {
  DCHECK(key);
  DCHECK(value);
  if (segments.history_segments_size() == 0) {
    return false;
  }

  const Segment &history_segment =
      segments.history_segment(segments.history_segments_size() - 1);
  if (history_segment.candidates_size() == 0) {
    return false;
  }

  key->assign(history_segment.candidate(0).key);
  value->assign(history_segment.candidate(0).value);
  return true;
}

void DictionaryPredictor::SetPredictionCost(const Segments &segments,
                                            vector<Result> *results) const {
  DCHECK(results);

  int rid = 0;  // 0 (BOS) is default
  if (segments.history_segments_size() > 0) {
    const Segment &history_segment =
        segments.history_segment(segments.history_segments_size() - 1);
    if (history_segment.candidates_size() > 0) {
      rid = history_segment.candidate(0).rid;  // use history segment's id
    }
  }

  const string &input_key = segments.conversion_segment(0).key();
  string history_key, history_value;
  GetHistoryKeyAndValue(segments, &history_key, &history_value);
  const string bigram_key = history_key + input_key;
  const bool is_suggestion = (segments.request_type() ==
                              Segments::SUGGESTION);

  // use the same scoring function for both unigram/bigram.
  // Bigram will be boosted because we pass the previous
  // key as a context information.
  const size_t bigram_key_len = Util::CharsLen(bigram_key);
  const size_t unigram_key_len = Util::CharsLen(input_key);

  // In the loop below, we track the minimum cost among those REALTIME
  // candidates that have the same key length as |input_key| so that we can set
  // a slightly smaller cost to REALTIME_TOP than these.
  int realtime_cost_min = kInfinity;
  Result *realtime_top_result = NULL;

  for (size_t i = 0; i < results->size(); ++i) {
    const Result &result = results->at(i);

    // The cost of REALTIME_TOP is determined after the loop based on the
    // minimum cost for REALTIME. Just remember the pointer of result.
    if (result.types & REALTIME_TOP) {
      realtime_top_result = &results->at(i);
      continue;
    }

    const int cost = GetLMCost(result, rid);
    const size_t query_len =
        (result.types & BIGRAM) ? bigram_key_len : unigram_key_len;
    const size_t key_len = Util::CharsLen(result.key);

    if (IsAggressiveSuggestion(query_len, key_len, cost,
                               is_suggestion, results->size())) {
      results->at(i).cost = kInfinity;
      continue;
    }

    // cost = -500 * log(lang_prob(w) * (1 + remain_length))    -- (1)
    // where lang_prob(w) is a language model probability of the word "w", and
    // remain_length the length of key user must type to input "w".
    //
    // Example:
    // key/value = "とうきょう/東京"
    // user_input = "とう"
    // remain_length = len("とうきょう") - len("とう") = 3
    //
    // By taking the log of (1),
    // cost  = -500 [log(lang_prob(w)) + log(1 + ramain_length)]
    //       = -500 * log(lang_prob(w)) + 500 * log(1 + remain_length)
    //       = cost - 500 * log(1 + remain_length)
    // Because 500 * log(lang_prob(w)) = -cost.
    //
    // lang_prob(w) * (1 + remain_length) represents how user can reduce
    // the total types by choosing this candidate.
    // Before this simple algorithm, we have been using an SVM-base scoring,
    // but we stop usign it with the following reasons.
    // 1) Hard to maintain the ranking.
    // 2) Hard to control the final results of SVM.
    // 3) Hard to debug.
    // 4) Since we used the log(remain_length) as a feature,
    //    the new ranking algorithm and SVM algorithm was essentially
    //    the same.
    // 5) Since we used the length of value as a feature, we find
    //    inconsistencies between the conversion and the prediction
    //    -- the results of top prediction and the top conversion
    //    (the candidate shown after the space key) may differ.
    //
    // The new function brings consistent results. If two candidate
    // have the same reading (key), they should have the same cost bonus
    // from the length part. This implies that the result is reranked by
    // the language model probability as long as the key part is the same.
    // This behavior is baisically the same as the converter.
    //
    // TODO(team): want find the best parameter instread of kCostFactor.
    const int kCostFactor = 500;
    results->at(i).cost = cost -
        kCostFactor * log(1.0 + max(0, static_cast<int>(key_len - query_len)));

    // Update the minimum cost for REALTIME candidates that have the same key
    // length as input_key.
    if (result.types & REALTIME &&
        result.cost < realtime_cost_min &&
        result.key.size() == input_key.size()) {
      realtime_cost_min = result.cost;
    }
  }

  // Ensure that the REALTIME_TOP candidate has relatively smaller cost than
  // those of REALTIME candidates.
  if (realtime_top_result != NULL) {
    realtime_top_result->cost = max(0, realtime_cost_min - 10);
  }
}

void DictionaryPredictor::SetLMCost(const Segments &segments,
                                    vector<Result> *results) const {
  DCHECK(results);

  // ranking for mobile
  int rid = 0;  // 0 (BOS) is default
  int prev_cost = 0;
  if (segments.history_segments_size() > 0) {
    const Segment &history_segment =
        segments.history_segment(segments.history_segments_size() - 1);
    if (history_segment.candidates_size() > 0) {
      rid = history_segment.candidate(0).rid;  // use history segment's id
      prev_cost = history_segment.candidate(0).cost;
      if (prev_cost == 0) {
        // if prev_cost is set to be 0 for some reason, use default cost.
        prev_cost = 5000;
      }
    }
  }

  const size_t input_key_len = Util::CharsLen(
      segments.conversion_segment(0).key());
  for (size_t i = 0; i < results->size(); ++i) {
    const Result &result = results->at(i);

    int cost = GetLMCost(result, rid);
    // Demote filtered word here, because they are not filtered for exact match.
    // Even for exact match, we don't want to show aggressive words
    // with high ranking.
    if (suggestion_filter_->IsBadSuggestion(result.value)) {
      // Cost penalty means for bad suggestion.
      // 3453 = 500 * log(1000)
      const int kBadSuggestionPenalty = 3453;
      cost += kBadSuggestionPenalty;
    }

    // Make exact candidates to have higher ranking.
    // Because for mobile, suggestion is the main candidates and
    // users expect the candidates for the input key on the candidates.
    if (result.types & (UNIGRAM | TYPING_CORRECTION)) {
      const size_t key_len = Util::CharsLen(result.key);
      if (key_len > input_key_len) {
        // Cost penalty means that exact candiates are evaluated
        // 50 times bigger in frequency.
        // Note that the cost is calculated by cost = -500 * log(prob)
        // 1956 = 500 * log(50)
        const int kNotExactPenalty = 1956;
        cost += kNotExactPenalty;
      }
    }
    if (result.types & BIGRAM) {
      // When user inputs "六本木" and there is an entry
      // "六本木ヒルズ" in the dictionary, we can suggest
      // "ヒルズ" as a ZeroQuery suggestion. In this case,
      // We can't calcurate the transition cost between "六本木"
      // and "ヒルズ". If we ignore the transition cost,
      // bigram-based suggestion will be overestimated.
      // Here we use |default_transition_cost| as an
      // transition cost between "六本木" and "ヒルズ". Currently,
      // the cost is basically the same as the cost between
      // "名詞,一般" and "名詞,一般".
      const int kDefaultTransitionCost = 1347;
      // Promoting bigram candidates.
      const int kBigramBonus = 800;  // ~= 500*ln(5)
      cost += (kDefaultTransitionCost - kBigramBonus - prev_cost);
    }
    results->at(i).cost = cost;
  }
}

void DictionaryPredictor::ApplyPenaltyForKeyExpansion(
    const Segments &segments, vector<Result> *results) const {
  if (segments.conversion_segments_size() == 0) {
    return;
  }
  // Cost penalty 1151 means that expanded candiates are evaluated
  // 10 times smaller in frequency.
  // Note that the cost is calcurated by cost = -500 * log(prob)
  // 1151 = 500 * log(10)
  const int kKeyExpansionPenalty = 1151;
  const string &conversion_key = segments.conversion_segment(0).key();
  for (size_t i = 0; i < results->size(); ++i) {
    const Result &result = results->at(i);
    if (result.types & TYPING_CORRECTION) {
      continue;
    }
    if (!Util::StartsWith(result.key, conversion_key)) {
      results->at(i).cost += kKeyExpansionPenalty;
    }
  }
}

size_t DictionaryPredictor::GetMissSpelledPosition(
    const string &key, const string &value) const {
  string hiragana_value;
  Util::KatakanaToHiragana(value, &hiragana_value);
  // value is mixed type. return true if key == request_key.
  if (Util::GetScriptType(hiragana_value) != Util::HIRAGANA) {
    return Util::CharsLen(key);
  }

  // Find the first position of character where miss spell occurs.
  int position = 0;
  ConstChar32Iterator key_iter(key);
  for (ConstChar32Iterator hiragana_iter(hiragana_value);
       !hiragana_iter.Done() && !key_iter.Done();
       hiragana_iter.Next(), key_iter.Next(), ++position) {
    if (hiragana_iter.Get() != key_iter.Get()) {
      return position;
    }
  }

  // not find. return the length of key.
  while (!key_iter.Done()) {
    ++position;
    key_iter.Next();
  }

  return position;
}

void DictionaryPredictor::RemoveMissSpelledCandidates(
    size_t request_key_len,
    vector<Result> *results) const {
  DCHECK(results);

  if (results->size() <= 1) {
    return;
  }

  int spelling_correction_size = 5;
  for (size_t i = 0; i < results->size(); ++i) {
    const Result &result = (*results)[i];
    if (!(result.candidate_attributes &
          Segment::Candidate::SPELLING_CORRECTION)) {
      continue;
    }

    // Only checks at most 5 spelling corrections to avoid the case
    // like all candidates have SPELLING_CORRECTION.
    if (--spelling_correction_size == 0) {
      return;
    }

    vector<size_t> same_key_index, same_value_index;
    for (size_t j = 0; j < results->size(); ++j) {
      if (i == j) {
        continue;
      }
      const Result &target_result = (*results)[j];
      if (target_result.candidate_attributes &
          Segment::Candidate::SPELLING_CORRECTION) {
        continue;
      }
      if (target_result.key == result.key) {
        same_key_index.push_back(j);
      }
      if (target_result.value == result.value) {
        same_value_index.push_back(j);
      }
    }

    // delete same_key_index and same_value_index
    if (!same_key_index.empty() && !same_value_index.empty()) {
      results->at(i).types = NO_PREDICTION;
      for (size_t k = 0; k < same_key_index.size(); ++k) {
        results->at(same_key_index[k]).types = NO_PREDICTION;
      }
    } else if (same_key_index.empty() && !same_value_index.empty()) {
      results->at(i).types = NO_PREDICTION;
    } else if (!same_key_index.empty() && same_value_index.empty()) {
      for (size_t k = 0; k < same_key_index.size(); ++k) {
        results->at(same_key_index[k]).types = NO_PREDICTION;
      }
      if (request_key_len <= GetMissSpelledPosition(result.key, result.value)) {
        results->at(i).types = NO_PREDICTION;
      }
    }
  }
}

bool DictionaryPredictor::IsAggressiveSuggestion(
    size_t query_len, size_t key_len, int cost,
    bool is_suggestion, size_t total_candidates_size) const {
  // Temporal workaround for fixing the problem where longer sentence-like
  // suggestions are shown when user input is very short.
  // "ただしい" => "ただしいけめんにかぎる"
  // "それでもぼ" => "それでもぼくはやっていない".
  // If total_candidates_size is small enough, we don't perform
  // special filtering. e.g., "せんとち" has only two candidates, so
  // showing "千と千尋の神隠し" is OK.
  // Also, if the cost is too small (< 5000), we allow to display
  // long phrases. Examples include "よろしくおねがいします".
  if (is_suggestion && total_candidates_size >= 10 && key_len >= 8 &&
      cost >= 5000 && query_len <= static_cast<size_t>(0.4 * key_len)) {
    return true;
  }

  return false;
}

size_t DictionaryPredictor::GetRealtimeCandidateMaxSize(
    const Segments &segments, bool mixed_conversion, size_t max_size) const {
  const Segments::RequestType request_type = segments.request_type();
  DCHECK(request_type == Segments::PREDICTION ||
         request_type == Segments::SUGGESTION ||
         request_type == Segments::PARTIAL_PREDICTION ||
         request_type == Segments::PARTIAL_SUGGESTION);
  const int kFewResultThreshold = 8;
  size_t default_size = 10;
  if (segments.segments_size() > 0 &&
      Util::CharsLen(segments.segment(0).key()) >= kFewResultThreshold) {
    // We don't make so many realtime conversion prediction
    // even if we have enough margin, as it's expected less useful.
    max_size = min(max_size, static_cast<size_t>(8));
    default_size = 5;
  }
  size_t size = 0;
  switch (request_type) {
    case Segments::PREDICTION:
      size = mixed_conversion ? max_size : default_size;
      break;
    case Segments::SUGGESTION:
      // Fewer candidatats are needed basically.
      // But on mixed_conversion mode we should behave like as conversion mode.
      size = mixed_conversion ? default_size : 1;
      break;
    case Segments::PARTIAL_PREDICTION:
      // This is kind of prediction so richer result than PARTIAL_SUGGESTION
      // is needed.
      size = max_size;
      break;
    case Segments::PARTIAL_SUGGESTION:
      // PARTIAL_SUGGESTION works like as conversion mode so returning
      // some candidates is needed.
      size = default_size;
      break;
    default:
      size = 0;  // Never reach here
  }

  return min(max_size, size);
}

bool DictionaryPredictor::PushBackTopConversionResult(
    const ConversionRequest &request,
    const Segments &segments,
    vector<Result> *results) const {
  DCHECK_EQ(1, segments.conversion_segments_size());

  Segments tmp_segments;
  tmp_segments.CopyFrom(segments);
  tmp_segments.set_max_conversion_candidates_size(20);
  ConversionRequest tmp_request;
  tmp_request.CopyFrom(request);
  tmp_request.set_composer_key_selection(ConversionRequest::PREDICTION_KEY);
  // Some rewriters cause significant performance loss. So we skip them.
  tmp_request.set_skip_slow_rewriters(true);
  // This method emulates usual converter's behavior so here disable
  // partial candidates.
  tmp_request.set_create_partial_candidates(false);
  if (!converter_->StartConversionForRequest(tmp_request, &tmp_segments)) {
    return false;
  }

  results->push_back(Result());
  Result *result = &results->back();
  result->key = segments.conversion_segment(0).key();
  result->lid = tmp_segments.conversion_segment(0).candidate(0).lid;
  result->rid = tmp_segments.conversion_segment(
      tmp_segments.conversion_segments_size() - 1).candidate(0).rid;
  result->SetTypesAndTokenAttributes(REALTIME | REALTIME_TOP, Token::NONE);
  result->candidate_attributes |= Segment::Candidate::NO_VARIANTS_EXPANSION;

  // Concatenate the top candidates.
  // Note that since StartConversionForRequest() runs in conversion mode, the
  // resulting |tmp_segments| doesn't have inner_segment_boundary. We need to
  // construct it manually here.
  // TODO(noriyukit): This is code duplicate in converter/nbest_generator.cc and
  // we should refactor code after finding more good design.
  bool inner_segment_boundary_success = true;
  for (size_t i = 0; i < tmp_segments.conversion_segments_size(); ++i) {
    const Segment &segment = tmp_segments.conversion_segment(i);
    const Segment::Candidate &candidate = segment.candidate(0);
    result->value.append(candidate.value);
    result->wcost += candidate.cost;

    uint32 encoded_lengths;
    if (inner_segment_boundary_success &&
        Segment::Candidate::EncodeLengths(candidate.key.size(),
                                          candidate.value.size(),
                                          candidate.content_key.size(),
                                          candidate.content_value.size(),
                                          &encoded_lengths)) {
      result->inner_segment_boundary.push_back(encoded_lengths);
    } else {
      inner_segment_boundary_success = false;
    }
  }
  if (!inner_segment_boundary_success) {
    LOG(WARNING) << "Failed to construct inner segment boundary";
    result->inner_segment_boundary.clear();
  }
  return true;
}

void DictionaryPredictor::AggregateRealtimeConversion(
    PredictionTypes types,
    const ConversionRequest &request,
    Segments *segments,
    vector<Result> *results) const {
  if (!(types & REALTIME)) {
    return;
  }

  DCHECK(converter_);
  DCHECK(immutable_converter_);
  DCHECK(segments);
  DCHECK(results);

  // TODO(noriyukit): Currently, |segments| is abused as a temporary output from
  // the immutable converter. Therefore, the first segment needs to be
  // mutable. Fix this bad abuse.
  Segment *segment = segments->mutable_conversion_segment(0);
  DCHECK(!segment->key().empty());

  // First insert a top conversion result.
  if (request.use_actual_converter_for_realtime_conversion()) {
    if (!PushBackTopConversionResult(request, *segments, results)) {
      LOG(WARNING) << "Realtime conversion with converter failed";
    }
  }

  // In what follows, add results from immutable converter.
  // TODO(noriyukit): The |immutable_converter_| used below can be replaced by
  // |converter_| in principle.  There's a problem of ranking when we get
  // multiple segments, i.e., how to concatenate candidates in each
  // segment. Currently, immutable converter handles such ranking in prediction
  // mode to generate single segment results. So we want to share that code.

  // Preserve the current max_prediction_candidates_size and candidates_size to
  // restore them at the end of this method.
  const size_t prev_candidates_size = segment->candidates_size();
  const size_t prev_max_prediction_candidates_size =
      segments->max_prediction_candidates_size();

  // Set how many candidates we want to obtain with the immutable
  // converter.
  const bool mixed_conversion = IsMixedConversionEnabled(request.request());
  size_t realtime_candidates_size = GetRealtimeCandidateMaxSize(
      *segments,
      mixed_conversion,
      prev_max_prediction_candidates_size - prev_candidates_size);
  if (realtime_candidates_size == 0) {
    return;
  }

  segments->set_max_prediction_candidates_size(
      prev_candidates_size + realtime_candidates_size);

  if (!immutable_converter_->ConvertForRequest(request, segments) ||
      prev_candidates_size >= segment->candidates_size()) {
    LOG(WARNING) << "Convert failed";
    return;
  }

  // A little tricky treatment:
  // Since ImmutableConverter::Convert creates a set of new candidates,
  // copy them into the array of Results.
  for (size_t i = prev_candidates_size;
       i < segment->candidates_size(); ++i) {
    const Segment::Candidate &candidate = segment->candidate(i);
    results->push_back(Result());
    Result *result = &results->back();
    result->key = candidate.key;
    result->value = candidate.value;
    result->wcost = candidate.wcost;
    result->lid = candidate.lid;
    result->rid = candidate.rid;
    result->inner_segment_boundary = candidate.inner_segment_boundary;
    result->SetTypesAndTokenAttributes(REALTIME, Token::NONE);
    result->candidate_attributes |= candidate.attributes;
    result->consumed_key_size = candidate.consumed_key_size;
  }
  // Remove candidates created by ImmutableConverter.
  segment->erase_candidates(prev_candidates_size,
                            segment->candidates_size() -
                            prev_candidates_size);
  // Restore the max_prediction_candidates_size.
  segments->set_max_prediction_candidates_size(
      prev_max_prediction_candidates_size);
}

size_t DictionaryPredictor::GetCandidateCutoffThreshold(
    const Segments &segments) const {
  DCHECK(segments.request_type() == Segments::PREDICTION ||
         segments.request_type() == Segments::SUGGESTION);
  if (segments.request_type() == Segments::PREDICTION) {
    // If PREDICTION, many candidates are needed than SUGGESTION.
    return kPredictionMaxResultsSize;
  }
  return kSuggestionMaxResultsSize;
}

void DictionaryPredictor::AggregateUnigramPrediction(
    PredictionTypes types,
    const ConversionRequest &request,
    const Segments &segments,
    vector<Result> *results) const {
  if (!(types & UNIGRAM)) {
    return;
  }

  DCHECK(results);
  DCHECK(segments.request_type() == Segments::PREDICTION ||
         segments.request_type() == Segments::SUGGESTION);

  const bool mixed_conversion = IsMixedConversionEnabled(request.request());
  if (!mixed_conversion) {
    AggregateUnigramCandidate(request, segments, results);
  } else {
    AggregateUnigramCandidateForMixedConversion(request, segments, results);
  }
}

void DictionaryPredictor::AggregateUnigramCandidate(
    const ConversionRequest &request,
    const Segments &segments,
    vector<Result> *results) const {
  DCHECK(results);
  DCHECK(dictionary_);

  const size_t cutoff_threshold = GetCandidateCutoffThreshold(segments);
  const size_t prev_results_size = results->size();
  GetPredictiveResults(*dictionary_, "", request, segments, UNIGRAM,
                       cutoff_threshold, results);
  const size_t unigram_results_size = results->size() - prev_results_size;

  // If size reaches max_results_size (== cutoff_threshold).
  // we don't show the candidates, since disambiguation from
  // 256 candidates is hard. (It may exceed max_results_size, because this is
  // just a limit for each backend, so total number may be larger)
  if (unigram_results_size >= cutoff_threshold) {
    results->resize(prev_results_size);
  }
}

void DictionaryPredictor::AggregateUnigramCandidateForMixedConversion(
    const ConversionRequest &request,
    const Segments &segments,
    vector<Result> *results) const {
  const size_t cutoff_threshold = kPredictionMaxResultsSize;

  vector<Result> raw_result;
  // No history key
  GetPredictiveResults(*dictionary_, "", request, segments, UNIGRAM,
                       cutoff_threshold, &raw_result);

  // Hereafter, we split "Needed Results" and "(maybe) Unneeded Results."
  // The algorithm is:
  // 1) Take the Result with minimum cost.
  // 2) Remove results which is "redundant" (defined by MaybeRedundant),
  //    from remaining results.
  // 3) Repeat 1) and 2) five times.
  // Note: to reduce the number of memory allocation, we swap out the
  //   "redundant" results to the end of the |results| vector.
  const size_t kDeleteTrialNum = 5;

  // min_iter is the beginning of the remaining results (inclusive), and
  // max_iter is the end of the remaining results (exclusive).
  typedef vector<Result>::iterator Iter;
  Iter min_iter = raw_result.begin();
  Iter max_iter = raw_result.end();
  for (size_t i = 0; i < kDeleteTrialNum; ++i) {
    if (min_iter == max_iter) {
      break;
    }

    // Find the Result with minimum cost. Swap it with the beginning element.
    std::iter_swap(min_iter,
                   std::min_element(min_iter, max_iter, ResultWCostLess()));

    const Result &reference_result = *min_iter;

    // Preserve the reference result.
    ++min_iter;

    // Traverse all remaining elements and check if each result is redundant.
    for (Iter iter = min_iter; iter != max_iter; ) {
      if (MaybeRedundant(reference_result.value, iter->value)) {
        // Swap out the redundant result.
        --max_iter;
        std::iter_swap(iter, max_iter);
      } else {
        ++iter;
      }
    }
  }

  // Then the |raw_result| contains;
  // [begin, min_iter): reference results in the above loop.
  // [max_iter, end): (maybe) redundant results.
  // [min_iter, max_iter): remaining results.
  // Here, we revive the redundant results up to five in the result cost order.
  const size_t kDoNotDeleteNum = 5;
  if (std::distance(max_iter, raw_result.end()) >= kDoNotDeleteNum) {
    std::partial_sort(max_iter, max_iter + kDoNotDeleteNum, raw_result.end(),
                      ResultWCostLess());
    max_iter += kDoNotDeleteNum;
  } else {
    max_iter = raw_result.end();
  }

  // Finally output the result.
  results->insert(results->end(), raw_result.begin(), max_iter);
}

void DictionaryPredictor::AggregateBigramPrediction(
    PredictionTypes types,
    const ConversionRequest &request,
    const Segments &segments,
    vector<Result> *results) const {
  if (!(types & BIGRAM)) {
    return;
  }

  DCHECK(results);
  DCHECK(dictionary_);

  // TODO(toshiyuki): Support suggestion from the last 2 histories.
  //  ex) "六本木"+"ヒルズ"->"レジデンス"
  string history_key, history_value;
  if (!GetHistoryKeyAndValue(segments, &history_key, &history_value)) {
    return;
  }
  AddBigramResultsFromHistory(
      history_key, history_value, request, segments, results);
}

void DictionaryPredictor::AddBigramResultsFromHistory(
    const string &history_key,
    const string &history_value,
    const ConversionRequest &request,
    const Segments &segments,
    vector<Result> *results) const {
  // Check that history_key/history_value are in the dictionary.
  FindValueCallback find_history_callback(history_value);
  dictionary_->LookupPrefix(history_key, request, &find_history_callback);

  // History value is not found in the dictionary.
  // User may create this the history candidate from T13N or segment
  // expand/shrinkg operations.
  if (!find_history_callback.found()) {
    return;
  }

  const size_t cutoff_threshold = GetCandidateCutoffThreshold(segments);
  const size_t prev_results_size = results->size();
  GetPredictiveResultsForBigram(
      *dictionary_, history_key, history_value, request, segments, BIGRAM,
      cutoff_threshold, results);
  const size_t bigram_results_size = results->size() - prev_results_size;

  // if size reaches max_results_size,
  // we don't show the candidates, since disambiguation from
  // 256 candidates is hard. (It may exceed max_results_size, because this is
  // just a limit for each backend, so total number may be larger)
  if (bigram_results_size >= cutoff_threshold) {
    results->resize(prev_results_size);
    return;
  }

  // Obtain the character type of the last history value.
  const size_t history_value_size = Util::CharsLen(history_value);
  if (history_value_size == 0) {
    return;
  }

  const Util::ScriptType history_ctype = Util::GetScriptType(history_value);
  const Util::ScriptType last_history_ctype =
      Util::GetScriptType(Util::SubString(history_value,
                                          history_value_size - 1, 1));
  for (size_t i = prev_results_size; i < results->size(); ++i) {
    CheckBigramResult(find_history_callback.token(), history_ctype,
                      last_history_ctype, request, &(*results)[i]);
  }
}

// Filter out irrelevant bigrams. For example, we don't want to
// suggest "リカ" from the history "アメ".
void DictionaryPredictor::CheckBigramResult(
    const Token &history_token,
    const Util::ScriptType history_ctype,
    const Util::ScriptType last_history_ctype,
    const ConversionRequest &request,
    Result *result) const {
  DCHECK(result);

  const string &history_key = history_token.key;
  const string &history_value = history_token.value;
  const string key(result->key, history_key.size(),
                   result->key.size() - history_key.size());
  const string value(result->value, history_value.size(),
                     result->value.size() - history_value.size());

  // Don't suggest 0-length key/value.
  if (key.empty() || value.empty()) {
    result->types = NO_PREDICTION;
    return;
  }

  const Util::ScriptType ctype =
      Util::GetScriptType(Util::SubString(value, 0, 1));

  if (history_ctype == Util::KANJI &&
      ctype == Util::KATAKANA) {
    // Do not filter "六本木ヒルズ"
    return;
  }

  // If freq("アメ") < freq("アメリカ"), we don't
  // need to suggest it. As "アメリカ" should already be
  // suggested when user type "アメ".
  // Note that wcost = -500 * log(prob).
  if (ctype != Util::KANJI &&
      history_token.cost > result->wcost) {
    result->types = NO_PREDICTION;
    return;
  }

  // If character type doesn't change, this boundary might NOT
  // be a word boundary. If character type is HIRAGANA,
  // we don't trust it. If Katakana, only trust iif the
  // entire key is reasonably long.
  if (ctype == last_history_ctype &&
      (ctype == Util::HIRAGANA ||
       (ctype == Util::KATAKANA && Util::CharsLen(result->key) <= 5))) {
    result->types = NO_PREDICTION;
    return;
  }

  // The suggested key/value pair must exist in the dictionary.
  // For example, we don't want to suggest "ターネット" from
  // the history "イン".
  // If character type is Kanji and the suggestion is not a
  // zero_query_suggestion, we relax this condition, as there are
  // many Kanji-compounds which may not in the dictionary. For example,
  // we want to suggest "霊長類研究所" from the history "京都大学".
  if (ctype == Util::KANJI && Util::CharsLen(value) >= 2) {
    // Do not filter this.
    // TODO(toshiyuki): one-length kanji prediciton may be annoying other than
    // some exceptions, "駅", "口", etc
    return;
  }

  FindValueCallback callback(value);
  dictionary_->LookupPrefix(key, request, &callback);
  if (!callback.found()) {
    result->types = NO_PREDICTION;
    return;
  }
}

void DictionaryPredictor::GetPredictiveResults(
    const DictionaryInterface &dictionary,
    const string &history_key,
    const ConversionRequest &request,
    const Segments &segments,
    PredictionTypes types,
    size_t lookup_limit,
    vector<Result> *results) const {
  if (!request.has_composer() ||
      !FLAGS_enable_expansion_for_dictionary_predictor) {
    const string &query_key = segments.conversion_segment(0).key();
    string input_key = history_key;
    input_key.append(query_key);
    const bool is_zero_query = query_key.empty();
    PredictiveLookupCallback callback(types, lookup_limit, input_key.size(),
                                      NULL, is_zero_query, results);
    dictionary.LookupPredictive(input_key, request, &callback);
    return;
  }

  // If we have ambiguity for the input, get expanded key.
  // Example1 roman input: for "あk", we will get |base|, "あ" and |expanded|,
  // "か", "き", etc
  // Example2 kana input: for "あか", we will get |base|, "あ" and |expanded|,
  // "か", and "が".
  string base;
  set<string> expanded;
  request.composer().GetQueriesForPrediction(&base, &expanded);
  string input_key = history_key;
  input_key.append(base);
  const bool is_zero_query = base.empty();
  PredictiveLookupCallback callback(
      types, lookup_limit, input_key.size(),
      expanded.empty() ? NULL : &expanded, is_zero_query, results);
  dictionary.LookupPredictive(input_key, request, &callback);
}

void DictionaryPredictor::GetPredictiveResultsForBigram(
    const DictionaryInterface &dictionary,
    const string &history_key,
    const string &history_value,
    const ConversionRequest &request,
    const Segments &segments,
    PredictionTypes types,
    size_t lookup_limit,
    vector<Result> *results) const {
  if (!request.has_composer() ||
      !FLAGS_enable_expansion_for_dictionary_predictor) {
    const string &query_key = segments.conversion_segment(0).key();
    string input_key = history_key;
    input_key.append(query_key);
    const bool is_zero_query = query_key.empty();
    PredictiveBigramLookupCallback callback(
        types, lookup_limit, input_key.size(), NULL, history_value,
        is_zero_query, results);
    dictionary.LookupPredictive(input_key, request, &callback);
    return;
  }

  // If we have ambiguity for the input, get expanded key.
  // Example1 roman input: for "あk", we will get |base|, "あ" and |expanded|,
  // "か", "き", etc
  // Example2 kana input: for "あか", we will get |base|, "あ" and |expanded|,
  // "か", and "が".
  string base;
  set<string> expanded;
  request.composer().GetQueriesForPrediction(&base, &expanded);
  string input_key = history_key;
  input_key.append(base);
  const bool is_zero_query = base.empty();
  PredictiveBigramLookupCallback callback(types, lookup_limit, input_key.size(),
                                          expanded.empty() ? NULL : &expanded,
                                          history_value, is_zero_query,
                                          results);
  dictionary.LookupPredictive(input_key, request, &callback);
}

void DictionaryPredictor::GetPredictiveResultsForEnglish(
    const DictionaryInterface &dictionary,
    const string &history_key,
    const ConversionRequest &request,
    const Segments &segments,
    PredictionTypes types,
    size_t lookup_limit,
    vector<Result> *results) const {
  if (!request.has_composer()) {
    GetPredictiveResults(dictionary, history_key, request, segments, types,
                         lookup_limit, results);
    return;
  }

  string input_key;
  request.composer().GetQueryForPrediction(&input_key);
  // We don't look up English words when key length is one.
  if (input_key.size() < 2) {
    return;
  }
  const size_t prev_results_size = results->size();
  if (Util::IsUpperAscii(input_key)) {
    // For upper case key, look up its lower case version and then transform the
    // results to upper case.
    string key(input_key);
    Util::LowerString(&key);
    PredictiveLookupCallback callback(types, lookup_limit, key.size(), NULL,
                                      false, results);
    dictionary.LookupPredictive(key, request, &callback);
    for (size_t i = prev_results_size; i < results->size(); ++i) {
      Util::UpperString(&results->at(i).value);
    }
  } else if (Util::IsCapitalizedAscii(input_key)) {
    // For capitalized key, look up its lower case version and then transform
    // the results to capital.
    string key(input_key);
    Util::LowerString(&key);
    PredictiveLookupCallback callback(types, lookup_limit, key.size(), NULL,
                                      false, results);
    dictionary.LookupPredictive(key, request, &callback);
    for (size_t i = prev_results_size; i < results->size(); ++i) {
      Util::CapitalizeString(&results->at(i).value);
    }
  } else {
    // For other cases (lower and as-is), just look up directly.
    PredictiveLookupCallback callback(types, lookup_limit, input_key.size(),
                                      NULL, false, results);
    dictionary.LookupPredictive(input_key, request, &callback);
  }
  // If input mode is FULL_ASCII, then convert the results to full-width.
  if (request.composer().GetInputMode() == transliteration::FULL_ASCII) {
    string tmp;
    for (size_t i = prev_results_size; i < results->size(); ++i) {
      tmp.assign(results->at(i).value);
      Util::HalfWidthAsciiToFullWidthAscii(tmp, &results->at(i).value);
    }
  }
}

void DictionaryPredictor::GetPredictiveResultsUsingTypingCorrection(
    const DictionaryInterface &dictionary,
    const string &history_key,
    const ConversionRequest &request,
    const Segments &segments,
    PredictionTypes types,
    size_t lookup_limit,
    vector<Result> *results) const {
  if (!request.has_composer()) {
    return;
  }

  vector<composer::TypeCorrectedQuery> queries;
  request.composer().GetTypeCorrectedQueriesForPrediction(&queries);
  for (size_t query_index = 0; query_index < queries.size(); ++query_index) {
    const composer::TypeCorrectedQuery &query = queries[query_index];
    const string input_key = history_key + query.base;
    const size_t previous_results_size = results->size();
    PredictiveLookupCallback callback(
        types, lookup_limit, input_key.size(),
        query.expanded.empty() ? NULL : &query.expanded, false, results);
    dictionary.LookupPredictive(input_key, request, &callback);

    for (size_t i = previous_results_size; i < results->size(); ++i) {
      results->at(i).wcost += query.cost;
    }
    lookup_limit -= results->size() - previous_results_size;
    if (lookup_limit <= 0) {
      break;
    }
  }
}

// static
bool DictionaryPredictor::GetZeroQueryCandidatesForKey(
    const ConversionRequest &request,
    const string &key, const ZeroQueryList *begin, const ZeroQueryList *end,
    vector<ZeroQueryResult> *results) {
  const int32 available_emoji_carrier =
      request.request().available_emoji_carrier();

  DCHECK(results);
  results->clear();
  const ZeroQueryList key_item = {key.c_str(), NULL, 0};
  const ZeroQueryList *result_rule =
      std::lower_bound(begin, end, key_item, ZeroQueryListCompare());
  if (result_rule == end || key != result_rule->key) {
    return false;
  }

  for (size_t i = 0; i < result_rule->entries_size; ++i) {
    const ZeroQueryEntry &entry = result_rule->entries[i];
    if (entry.type != ZERO_QUERY_EMOJI) {
      results->push_back(make_pair(entry.value, entry.type));
      continue;
    }
    if (available_emoji_carrier & Request::UNICODE_EMOJI &&
        entry.emoji_type & EMOJI_UNICODE) {
      results->push_back(make_pair(entry.value, entry.type));
      continue;
    }

    if ((available_emoji_carrier & Request::DOCOMO_EMOJI &&
         entry.emoji_type & EMOJI_DOCOMO) ||
        (available_emoji_carrier & Request::SOFTBANK_EMOJI &&
         entry.emoji_type & EMOJI_SOFTBANK) ||
        (available_emoji_carrier & Request::KDDI_EMOJI &&
         entry.emoji_type & EMOJI_KDDI)) {
      string android_pua;
      Util::UCS4ToUTF8(entry.emoji_android_pua, &android_pua);
      results->push_back(make_pair(android_pua, entry.type));
    }
  }
  return !results->empty();
}

// static
void DictionaryPredictor::AppendZeroQueryToResults(
    const vector<ZeroQueryResult> &candidates, uint16 lid, uint16 rid,
    vector<Result> *results) {
  int cost = 0;

  for (size_t i = 0; i < candidates.size(); ++i) {
    // Increment cost to show the candidates in order.
    const int kSuffixPenalty = 10;

    results->push_back(Result());
    Result *result = &results->back();
    result->SetTypesAndTokenAttributes(SUFFIX, Token::NONE);
    result->SetSourceInfoForZeroQuery(candidates[i].second);
    result->key = candidates[i].first;
    result->value = candidates[i].first;
    result->wcost = cost;
    result->lid = lid;
    result->rid = rid;

    cost += kSuffixPenalty;
  }
}

// Returns true if we add zero query result.
bool DictionaryPredictor::AggregateNumberZeroQueryPrediction(
    const ConversionRequest &request,
    const Segments &segments, vector<Result> *results) const {
  string number_key;
  if (!GetNumberHistory(segments, &number_key)) {
    return false;
  }

  vector<ZeroQueryResult> candidates_for_number_key;
  GetZeroQueryCandidatesForKey(request,
                               number_key,
                               kZeroQueryNum_data,
                               kZeroQueryNum_data + kZeroQueryNum_size,
                               &candidates_for_number_key);

  vector<ZeroQueryResult> default_candidates_for_number;
  GetZeroQueryCandidatesForKey(request,
                               "default",
                               kZeroQueryNum_data,
                               kZeroQueryNum_data + kZeroQueryNum_size,
                               &default_candidates_for_number);
  DCHECK(!default_candidates_for_number.empty());

  AppendZeroQueryToResults(candidates_for_number_key,
                           counter_suffix_word_id_,
                           counter_suffix_word_id_,
                           results);
  AppendZeroQueryToResults(default_candidates_for_number,
                           counter_suffix_word_id_,
                           counter_suffix_word_id_,
                           results);
  return true;
}

// Returns true if we add zero query result.
bool DictionaryPredictor::AggregateZeroQueryPrediction(
    const ConversionRequest &request,
    const Segments &segments, vector<Result> *results) const {
  const size_t history_size = segments.history_segments_size();
  if (history_size <= 0) {
    return false;
  }

  const Segment &last_segment = segments.history_segment(history_size - 1);
  DCHECK_GT(last_segment.candidates_size(), 0);
  const string &history_value = last_segment.candidate(0).value;

  vector<ZeroQueryResult> candidates;
  if (!GetZeroQueryCandidatesForKey(request,
                                    history_value,
                                    kZeroQueryData_data,
                                    kZeroQueryData_data + kZeroQueryData_size,
                                    &candidates)) {
    return false;
  }

  const uint16 kId = 0;  // EOS
  AppendZeroQueryToResults(candidates, kId, kId, results);
  return true;
}

void DictionaryPredictor::AggregateSuffixPrediction(
    PredictionTypes types,
    const ConversionRequest &request,
    const Segments &segments,
    vector<Result> *results) const {
  if (!(types & SUFFIX)) {
    return;
  }

  DCHECK_GT(segments.conversion_segments_size(), 0);

  const bool is_zero_query = segments.conversion_segment(0).key().empty();
  if (is_zero_query) {
    if (AggregateNumberZeroQueryPrediction(request, segments, results)) {
      return;
    }
    if (AggregateZeroQueryPrediction(request, segments, results)) {
      // Fall through
      // Appends normal suffix predictions
    }
    // Fall through
    // Use normal suffix predictions
  }

  const size_t cutoff_threshold = GetCandidateCutoffThreshold(segments);
  const string kEmptyHistoryKey = "";
  GetPredictiveResults(*suffix_dictionary_, kEmptyHistoryKey, request,
                       segments, SUFFIX, cutoff_threshold, results);
}

void DictionaryPredictor::AggregateEnglishPrediction(
    PredictionTypes types,
    const ConversionRequest &request,
    const Segments &segments,
    vector<Result> *results) const {
  if (!(types & ENGLISH)) {
    return;
  }
  DCHECK(results);
  DCHECK(dictionary_);

  const size_t cutoff_threshold = GetCandidateCutoffThreshold(segments);
  const size_t prev_results_size = results->size();

  // Currently, history key is never utilized.
  // TODO(noriyukit): Come up with a way of utilizing it.
  const string kEmptyHistoryKey = "";
  GetPredictiveResultsForEnglish(*dictionary_, kEmptyHistoryKey, request,
                                 segments, ENGLISH, cutoff_threshold, results);

  size_t unigram_results_size = results->size() - prev_results_size;
  if (unigram_results_size >= cutoff_threshold) {
    results->resize(prev_results_size);
    return;
  }
}

void DictionaryPredictor::AggregateTypeCorrectingPrediction(
    PredictionTypes types,
    const ConversionRequest &request,
    const Segments &segments,
    vector<Result> *results) const {
  if (!(types & TYPING_CORRECTION)) {
    return;
  }
  DCHECK(results);
  DCHECK(dictionary_);

  const size_t prev_results_size = results->size();
  if (prev_results_size > 10000) {
    return;
  }

  const size_t cutoff_threshold = GetCandidateCutoffThreshold(segments);

  // Currently, history key is never utilized.
  const string kEmptyHistoryKey = "";
  GetPredictiveResultsUsingTypingCorrection(
      *dictionary_, kEmptyHistoryKey, request, segments, TYPING_CORRECTION,
      cutoff_threshold, results);
  if (results->size() - prev_results_size >= cutoff_threshold) {
    results->resize(prev_results_size);
    return;
  }
}

DictionaryPredictor::PredictionTypes DictionaryPredictor::GetPredictionTypes(
    const ConversionRequest &request, const Segments &segments) {
  if (segments.request_type() == Segments::CONVERSION) {
    VLOG(2) << "request type is CONVERSION";
    return NO_PREDICTION;
  }

  if (segments.conversion_segments_size() < 1) {
    VLOG(2) << "segment size < 1";
    return NO_PREDICTION;
  }

  PredictionTypes result = NO_PREDICTION;

  // Check if realtime conversion should be used.
  if (ShouldRealTimeConversionEnabled(request, segments)) {
    result |= REALTIME;
  }

  const bool zero_query_suggestion = request.request().zero_query_suggestion();
  if (IsLatinInputMode(request) && !zero_query_suggestion) {
    if (request.config().use_dictionary_suggest()) {
      // By following the dictionary_suggest config, enable English prediction.
      result |= ENGLISH;
    }

    // Returns regardless of whether use_dictionary_suggest is enabled or not
    // in the config, in order to avoid full-width candidates of English words.
    return result;
  }

  if (!request.config().use_dictionary_suggest() &&
      segments.request_type() == Segments::SUGGESTION) {
    VLOG(2) << "no_dictionary_suggest";
    return result;
  }

  const string &key = segments.conversion_segment(0).key();
  const size_t key_len = Util::CharsLen(key);
  if (key_len == 0 && !zero_query_suggestion) {
    return result;
  }

  // Never trigger prediction if key looks like zip code.
  if (segments.request_type() == Segments::SUGGESTION &&
      DictionaryPredictor::IsZipCodeRequest(key) && key_len < 6) {
    return result;
  }

  const int kMinUnigramKeyLen = zero_query_suggestion ? 1 : 3;

  // unigram based suggestion requires key_len >= kMinUnigramKeyLen.
  // Providing suggestions from very short user input key is annoying.
  if ((segments.request_type() == Segments::PREDICTION && key_len >= 1) ||
      key_len >= kMinUnigramKeyLen) {
    result |= UNIGRAM;
  }

  const size_t history_segments_size = segments.history_segments_size();
  if (history_segments_size > 0) {
    const Segment &history_segment =
        segments.history_segment(history_segments_size - 1);
    const int kMinHistoryKeyLen = zero_query_suggestion ? 2 : 3;
    // even in PREDICTION mode, bigram-based suggestion requires that
    // the length of previous key is >= kMinBigramKeyLen.
    // It also implies that bigram-based suggestion will be triggered,
    // even if the current key length is short enough.
    // TOOD(taku): this setting might be aggressive if the current key
    // looks like Japanese particle like "が|で|は"
    // If the current key looks like particle, we can make the behavior
    // less aggressive.
    if (history_segment.candidates_size() > 0 &&
        Util::CharsLen(history_segment.candidate(0).key) >= kMinHistoryKeyLen) {
      result |= BIGRAM;
    }
  }

  if (history_segments_size > 0 && zero_query_suggestion) {
    result |= SUFFIX;
  }

  if (IsTypingCorrectionEnabled(request) && key_len >= 3) {
    result |= TYPING_CORRECTION;
  }

  return result;
}

bool DictionaryPredictor::ShouldRealTimeConversionEnabled(
    const ConversionRequest &request,
    const Segments &segments) {
  const size_t kMaxRealtimeKeySize = 300;   // 300 bytes in UTF8
  const string &key = segments.conversion_segment(0).key();
  if (key.empty() || key.size() >= kMaxRealtimeKeySize) {
    // 1) If key is empty, realtime conversion doesn't work.
    // 2) If the key is too long, we'll hit a performance issue.
    return false;
  }

  return (segments.request_type() == Segments::PARTIAL_SUGGESTION ||
          request.config().use_realtime_conversion() ||
          IsMixedConversionEnabled(request.request()));
}

bool DictionaryPredictor::IsZipCodeRequest(const string &key) {
  if (key.empty()) {
    return false;
  }

  for (ConstChar32Iterator iter(key); !iter.Done(); iter.Next()) {
    const char32 c = iter.Get();
    if (!('0' <= c && c <= '9') && (c != '-')) {
      return false;
    }
  }
  return true;
}

}  // namespace mozc
