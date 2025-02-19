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
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/flags.h"
#include "base/logging.h"
#include "base/port.h"
#include "base/singleton.h"
#include "base/system_util.h"
#include "base/util.h"
#include "composer/composer.h"
#include "composer/internal/typing_model.h"
#include "composer/table.h"
#include "config/config_handler.h"
#include "converter/connector.h"
#include "converter/converter_interface.h"
#include "converter/converter_mock.h"
#include "converter/immutable_converter.h"
#include "converter/immutable_converter_interface.h"
#include "converter/node_allocator.h"
#include "converter/segmenter.h"
#include "converter/segments.h"
#include "data_manager/data_manager_interface.h"
#include "data_manager/testing/mock_data_manager.h"
#include "dictionary/dictionary_interface.h"
#include "dictionary/dictionary_mock.h"
#include "dictionary/pos_group.h"
#include "dictionary/pos_matcher.h"
#include "dictionary/suffix_dictionary.h"
#include "dictionary/suffix_dictionary_token.h"
#include "dictionary/suppression_dictionary.h"
#include "dictionary/system/system_dictionary.h"
#include "prediction/suggestion_filter.h"
#include "prediction/zero_query_list.h"
#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"
#include "request/conversion_request.h"
#include "session/request_test_util.h"
#include "testing/base/public/gmock.h"
#include "testing/base/public/googletest.h"
#include "testing/base/public/gunit.h"
#include "transliteration/transliteration.h"
#include "usage_stats/usage_stats.h"
#include "usage_stats/usage_stats_testing_util.h"

using std::unique_ptr;

using mozc::dictionary::DictionaryInterface;
using mozc::dictionary::DictionaryMock;
using mozc::dictionary::POSMatcher;
using mozc::dictionary::PosGroup;
using mozc::dictionary::SuffixDictionary;
using mozc::dictionary::SuffixToken;
using mozc::dictionary::SuppressionDictionary;
using mozc::dictionary::Token;
using testing::_;

DECLARE_string(test_tmpdir);
DECLARE_bool(enable_expansion_for_dictionary_predictor);

namespace mozc {
namespace {

const int kInfinity = (2 << 20);

DictionaryInterface *CreateSystemDictionaryFromDataManager(
    const DataManagerInterface &data_manager) {
  const char *data = NULL;
  int size = 0;
  data_manager.GetSystemDictionaryData(&data, &size);
  using mozc::dictionary::SystemDictionary;
  return SystemDictionary::Builder(data, size).Build();
}

DictionaryInterface *CreateSuffixDictionaryFromDataManager(
    const DataManagerInterface &data_manager) {
  const SuffixToken *tokens = NULL;
  size_t size = 0;
  data_manager.GetSuffixDictionaryData(&tokens, &size);
  return new SuffixDictionary(tokens, size);
}

SuggestionFilter *CreateSuggestionFilter(
    const DataManagerInterface &data_manager) {
  const char *data = NULL;
  size_t size = 0;
  data_manager.GetSuggestionFilterData(&data, &size);
  return new SuggestionFilter(data, size);
}

// Simple immutable converter mock for the realtime conversion test
class ImmutableConverterMock : public ImmutableConverterInterface {
 public:
  ImmutableConverterMock() {
    Segment *segment = segments_.add_segment();
    // "わたしのなまえはなかのです"
    segment->set_key("\xe3\x82\x8f\xe3\x81\x9f\xe3\x81\x97\xe3\x81\xae"
                     "\xe3\x81\xaa\xe3\x81\xbe\xe3\x81\x88\xe3\x81\xaf"
                     "\xe3\x81\xaa\xe3\x81\x8b\xe3\x81\xae\xe3\x81\xa7"
                     "\xe3\x81\x99");
    Segment::Candidate *candidate = segment->add_candidate();
    // "私の名前は中野です"
    candidate->value = "\xe7\xa7\x81\xe3\x81\xae\xe5\x90\x8d\xe5\x89\x8d"
        "\xe3\x81\xaf\xe4\xb8\xad\xe9\x87\x8e\xe3\x81\xa7\xe3\x81\x99";
    // "わたしのなまえはなかのです"
    candidate->key = ("\xe3\x82\x8f\xe3\x81\x9f\xe3\x81\x97\xe3\x81\xae"
                      "\xe3\x81\xaa\xe3\x81\xbe\xe3\x81\x88\xe3\x81\xaf"
                      "\xe3\x81\xaa\xe3\x81\x8b\xe3\x81\xae\xe3\x81\xa7"
                      "\xe3\x81\x99");
    // "わたしの, 私の", "わたし, 私"
    candidate->PushBackInnerSegmentBoundary(12, 6, 9, 3);
    // "なまえは, 名前は", "なまえ, 名前"
    candidate->PushBackInnerSegmentBoundary(12, 9, 9, 6);
    // "なかのです, 中野です", "なかの, 中野"
    candidate->PushBackInnerSegmentBoundary(15, 12, 9, 6);
  }

  virtual bool ConvertForRequest(
      const ConversionRequest &request, Segments *segments) const {
    segments->CopyFrom(segments_);
    return true;
  }

 private:
  Segments segments_;
};

class TestableDictionaryPredictor : public DictionaryPredictor {
  // Test-only subclass: Just changing access levels
 public:
  TestableDictionaryPredictor(
      const ConverterInterface *converter,
      const ImmutableConverterInterface *immutable_converter,
      const DictionaryInterface *dictionary,
      const DictionaryInterface *suffix_dictionary,
      const Connector *connector,
      const Segmenter *segmenter,
      const POSMatcher *pos_matcher,
      const SuggestionFilter *suggestion_filter)
      : DictionaryPredictor(converter,
                            immutable_converter,
                            dictionary,
                            suffix_dictionary,
                            connector,
                            segmenter,
                            pos_matcher,
                            suggestion_filter) {}

  using DictionaryPredictor::PredictionTypes;
  using DictionaryPredictor::NO_PREDICTION;
  using DictionaryPredictor::UNIGRAM;
  using DictionaryPredictor::BIGRAM;
  using DictionaryPredictor::REALTIME;
  using DictionaryPredictor::REALTIME_TOP;
  using DictionaryPredictor::SUFFIX;
  using DictionaryPredictor::ENGLISH;
  using DictionaryPredictor::Result;
  using DictionaryPredictor::MakeEmptyResult;
  using DictionaryPredictor::AddPredictionToCandidates;
  using DictionaryPredictor::AggregateRealtimeConversion;
  using DictionaryPredictor::AggregateUnigramPrediction;
  using DictionaryPredictor::AggregateBigramPrediction;
  using DictionaryPredictor::AggregateSuffixPrediction;
  using DictionaryPredictor::AggregateEnglishPrediction;
  using DictionaryPredictor::ApplyPenaltyForKeyExpansion;
  using DictionaryPredictor::TYPING_CORRECTION;
  using DictionaryPredictor::AggregateTypeCorrectingPrediction;
};

// Helper class to hold dictionary data and predictor objects.
class MockDataAndPredictor {
 public:
  // Initializes predictor with given dictionary and suffix_dictionary.  When
  // NULL is passed to the first argument |dictionary|, the default
  // DictionaryMock is used. For the second, the default is MockDataManager's
  // suffix dictionary. Note that |dictionary| is owned by this class but
  // |suffix_dictionary| is NOT owned because the current design assumes that
  // suffix dictionary is singleton.
  void Init(const DictionaryInterface *dictionary = NULL,
            const DictionaryInterface *suffix_dictionary = NULL) {
    testing::MockDataManager data_manager;

    pos_matcher_ = data_manager.GetPOSMatcher();
    suppression_dictionary_.reset(new SuppressionDictionary);
    if (!dictionary) {
      dictionary_mock_ = new DictionaryMock;
      dictionary_.reset(dictionary_mock_);
    } else {
      dictionary_mock_ = NULL;
      dictionary_.reset(dictionary);
    }
    if (!suffix_dictionary) {
      suffix_dictionary_.reset(
          CreateSuffixDictionaryFromDataManager(data_manager));
    } else {
      suffix_dictionary_.reset(suffix_dictionary);
    }
    CHECK(suffix_dictionary_.get());

    connector_.reset(Connector::CreateFromDataManager(data_manager));
    CHECK(connector_.get());

    segmenter_.reset(Segmenter::CreateFromDataManager(data_manager));
    CHECK(segmenter_.get());

    pos_group_.reset(new PosGroup(data_manager.GetPosGroupData()));
    suggestion_filter_.reset(CreateSuggestionFilter(data_manager));
    immutable_converter_.reset(
        new ImmutableConverterImpl(dictionary_.get(),
                                   suffix_dictionary_.get(),
                                   suppression_dictionary_.get(),
                                   connector_.get(),
                                   segmenter_.get(),
                                   pos_matcher_,
                                   pos_group_.get(),
                                   suggestion_filter_.get()));
    converter_.reset(new ConverterMock());
    dictionary_predictor_.reset(
        new TestableDictionaryPredictor(converter_.get(),
                                        immutable_converter_.get(),
                                        dictionary_.get(),
                                        suffix_dictionary_.get(),
                                        connector_.get(),
                                        segmenter_.get(),
                                        data_manager.GetPOSMatcher(),
                                        suggestion_filter_.get()));
  }

  const POSMatcher &pos_matcher() const {
    return *pos_matcher_;
  }

  DictionaryMock *mutable_dictionary() {
    return dictionary_mock_;
  }

  ConverterMock *mutable_converter_mock() {
    return converter_.get();
  }

  const TestableDictionaryPredictor *dictionary_predictor() {
    return dictionary_predictor_.get();
  }

  TestableDictionaryPredictor *mutable_dictionary_predictor() {
    return dictionary_predictor_.get();
  }

 private:
  const POSMatcher *pos_matcher_;
  unique_ptr<SuppressionDictionary> suppression_dictionary_;
  unique_ptr<const Connector> connector_;
  unique_ptr<const Segmenter> segmenter_;
  unique_ptr<const DictionaryInterface> suffix_dictionary_;
  unique_ptr<const DictionaryInterface> dictionary_;
  DictionaryMock *dictionary_mock_;
  unique_ptr<const PosGroup> pos_group_;
  unique_ptr<ImmutableConverterInterface> immutable_converter_;
  unique_ptr<ConverterMock> converter_;
  unique_ptr<const SuggestionFilter> suggestion_filter_;
  unique_ptr<TestableDictionaryPredictor> dictionary_predictor_;
};

class CallCheckDictionary : public DictionaryInterface {
 public:
  CallCheckDictionary() {}
  virtual ~CallCheckDictionary() {}

  MOCK_CONST_METHOD1(HasKey,
                     bool(StringPiece));
  MOCK_CONST_METHOD1(HasValue,
                     bool(StringPiece));
  MOCK_CONST_METHOD3(LookupPredictive,
                     void(StringPiece key,
                          const ConversionRequest& convreq,
                          Callback *callback));
  MOCK_CONST_METHOD3(LookupPrefix,
                     void(StringPiece key,
                          const ConversionRequest& convreq,
                          Callback *callback));
  MOCK_CONST_METHOD3(LookupExact,
                     void(StringPiece key,
                          const ConversionRequest& convreq,
                          Callback *callback));
  MOCK_CONST_METHOD3(LookupReverse,
                     void(StringPiece str,
                          const ConversionRequest& convreq,
                          Callback *callback));
};

// Action to call the third argument of LookupPrefix with the token
// <key, value>.
ACTION_P4(LookupPrefixOneToken, key, value, lid, rid) {
  Token token;
  token.key = key;
  token.value = value;
  token.lid = lid;
  token.rid = rid;
  arg2->OnToken(key, key, token);
}

void MakeSegmentsForSuggestion(const string key, Segments *segments) {
  segments->Clear();
  segments->set_max_prediction_candidates_size(10);
  segments->set_request_type(Segments::SUGGESTION);
  Segment *seg = segments->add_segment();
  seg->set_key(key);
  seg->set_segment_type(Segment::FREE);
}

void MakeSegmentsForPrediction(const string key, Segments *segments) {
  segments->Clear();
  segments->set_max_prediction_candidates_size(50);
  segments->set_request_type(Segments::PREDICTION);
  Segment *seg = segments->add_segment();
  seg->set_key(key);
  seg->set_segment_type(Segment::FREE);
}

void PrependHistorySegments(const string &key,
                            const string &value,
                            Segments *segments) {
  Segment *seg = segments->push_front_segment();
  seg->set_segment_type(Segment::HISTORY);
  seg->set_key(key);
  Segment::Candidate *c = seg->add_candidate();
  c->key = key;
  c->content_key = key;
  c->value = value;
  c->content_value = value;
}

class MockTypingModel : public mozc::composer::TypingModel {
 public:
  MockTypingModel() : TypingModel(NULL, 0, NULL, 0, NULL) {}
  ~MockTypingModel() {}
  int GetCost(StringPiece key) const {
    return 10;
  }
};

}  // namespace

class DictionaryPredictorTest : public ::testing::Test {
 public:
  DictionaryPredictorTest() :
      default_expansion_flag_(
          FLAGS_enable_expansion_for_dictionary_predictor) {
  }

  virtual ~DictionaryPredictorTest() {
    FLAGS_enable_expansion_for_dictionary_predictor = default_expansion_flag_;
  }

 protected:
  virtual void SetUp() {
    FLAGS_enable_expansion_for_dictionary_predictor = false;
    SystemUtil::SetUserProfileDirectory(FLAGS_test_tmpdir);
    request_.reset(new commands::Request);
    config_.reset(new config::Config);
    config::ConfigHandler::GetDefaultConfig(config_.get());
    table_.reset(new composer::Table);
    composer_.reset(
        new composer::Composer(table_.get(), request_.get(), config_.get()));
    convreq_.reset(
        new ConversionRequest(composer_.get(), request_.get(), config_.get()));

    mozc::usage_stats::UsageStats::ClearAllStatsForTest();
  }

  virtual void TearDown() {
    FLAGS_enable_expansion_for_dictionary_predictor = false;
    mozc::usage_stats::UsageStats::ClearAllStatsForTest();
  }

  static void AddWordsToMockDic(DictionaryMock *mock) {
    // "ぐーぐるあ"
    const char kGoogleA[] = "\xE3\x81\x90\xE3\x83\xBC\xE3\x81\x90\xE3\x82\x8B"
        "\xE3\x81\x82";

    const char kGoogleAdsenseHiragana[] = "\xE3\x81\x90\xE3\x83\xBC\xE3\x81\x90"
        "\xE3\x82\x8B\xE3\x81\x82\xE3\x81\xA9"
        "\xE3\x81\x9B\xE3\x82\x93\xE3\x81\x99";
    // "グーグルアドセンス"
    const char kGoogleAdsenseKatakana[] = "\xE3\x82\xB0\xE3\x83\xBC\xE3\x82\xB0"
        "\xE3\x83\xAB\xE3\x82\xA2\xE3\x83\x89"
        "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xB9";
    mock->AddLookupPredictive(kGoogleA, kGoogleAdsenseHiragana,
                              kGoogleAdsenseKatakana, Token::NONE);

    // "ぐーぐるあどわーず"
    const char kGoogleAdwordsHiragana[] =
        "\xE3\x81\x90\xE3\x83\xBC\xE3\x81\x90\xE3\x82\x8B"
        "\xE3\x81\x82\xE3\x81\xA9\xE3\x82\x8F\xE3\x83\xBC\xE3\x81\x9A";
    const char kGoogleAdwordsKatakana[] =
        "\xE3\x82\xB0\xE3\x83\xBC\xE3\x82\xB0\xE3\x83\xAB"
        "\xE3\x82\xA2\xE3\x83\x89\xE3\x83\xAF\xE3\x83\xBC\xE3\x82\xBA";
    mock->AddLookupPredictive(kGoogleA, kGoogleAdwordsHiragana,
                              kGoogleAdwordsKatakana, Token::NONE);

    // "ぐーぐる"
    const char kGoogle[] = "\xE3\x81\x90\xE3\x83\xBC"
        "\xE3\x81\x90\xE3\x82\x8B";
    mock->AddLookupPredictive(kGoogle, kGoogleAdsenseHiragana,
                              kGoogleAdsenseKatakana, Token::NONE);
    mock->AddLookupPredictive(kGoogle, kGoogleAdwordsHiragana,
                              kGoogleAdwordsKatakana, Token::NONE);

    // "グーグル"
    const char kGoogleKatakana[] = "\xE3\x82\xB0\xE3\x83\xBC"
        "\xE3\x82\xB0\xE3\x83\xAB";
    mock->AddLookupPrefix(kGoogle, kGoogleKatakana,
                          kGoogleKatakana, Token::NONE);

    // "あどせんす"
    const char kAdsense[] = "\xE3\x81\x82\xE3\x81\xA9\xE3\x81\x9B"
        "\xE3\x82\x93\xE3\x81\x99";
    const char kAdsenseKatakana[] = "\xE3\x82\xA2\xE3\x83\x89"
        "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xB9";
    mock->AddLookupPrefix(kAdsense, kAdsenseKatakana,
                          kAdsenseKatakana, Token::NONE);

    // "てすと"
    const char kTestHiragana[] = "\xE3\x81\xA6\xE3\x81\x99\xE3\x81\xA8";

    // "テスト"
    const char kTestKatakana[] = "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88";

    mock->AddLookupPrefix(kTestHiragana, kTestHiragana,
                          kTestKatakana, Token::NONE);

    // "ふぃるたーたいしょう"
    const char kFilterHiragana[] = "\xe3\x81\xb5\xe3\x81\x83\xe3\x82\x8b"
        "\xe3\x81\x9f\xe3\x83\xbc\xe3\x81\x9f\xe3\x81\x84\xe3\x81\x97"
        "\xe3\x82\x87\xe3\x81\x86";

    // "ふぃるたーたいし"
    const char kFilterPrefixHiragana[] = "\xe3\x81\xb5\xe3\x81\x83\xe3\x82\x8b"
        "\xe3\x81\x9f\xe3\x83\xbc\xe3\x81\x9f\xe3\x81\x84\xe3\x81\x97";

    // Note: This is in the filter
    // "フィルター対象"
    const char kFilterWord[] = "\xe3\x83\x95\xe3\x82\xa3\xe3\x83\xab"
        "\xe3\x82\xbf\xe3\x83\xbc\xe5\xaf\xbe\xe8\xb1\xa1";

    // Note: This is NOT in the filter
    // "フィルター大将"
    const char kNonFilterWord[] = "\xe3\x83\x95\xe3\x82\xa3\xe3\x83\xab"
        "\xe3\x82\xbf\xe3\x83\xbc\xe5\xa4\xa7\xe5\xb0\x86";

    mock->AddLookupPrefix(kFilterHiragana, kFilterHiragana,
                          kFilterWord, Token::NONE);

    mock->AddLookupPrefix(kFilterHiragana, kFilterHiragana,
                          kNonFilterWord, Token::NONE);

    mock->AddLookupPredictive(kFilterHiragana, kFilterHiragana,
                              kFilterWord, Token::NONE);

    mock->AddLookupPredictive(kFilterHiragana, kFilterPrefixHiragana,
                              kFilterWord, Token::NONE);

    // "かぷりちょうざ"
    const char kWrongCapriHiragana[] = "\xE3\x81\x8B\xE3\x81\xB7\xE3\x82\x8A"
        "\xE3\x81\xA1\xE3\x82\x87\xE3\x81\x86\xE3\x81\x96";

    // "かぷりちょーざ"
    const char kRightCapriHiragana[] = "\xE3\x81\x8B\xE3\x81\xB7\xE3\x82\x8A"
        "\xE3\x81\xA1\xE3\x82\x87\xE3\x83\xBC\xE3\x81\x96";

    // "カプリチョーザ"
    const char kCapriKatakana[] = "\xE3\x82\xAB\xE3\x83\x97\xE3\x83\xAA"
        "\xE3\x83\x81\xE3\x83\xA7\xE3\x83\xBC\xE3\x82\xB6";

    mock->AddLookupPrefix(kWrongCapriHiragana, kRightCapriHiragana,
                          kCapriKatakana, Token::SPELLING_CORRECTION);

    mock->AddLookupPredictive(kWrongCapriHiragana, kRightCapriHiragana,
                              kCapriKatakana, Token::SPELLING_CORRECTION);

    // "で"
    const char kDe[] = "\xE3\x81\xA7";

    mock->AddLookupPrefix(kDe, kDe, kDe, Token::NONE);

    // "ひろすえ/広末"
    const char kHirosueHiragana[] = "\xE3\x81\xB2\xE3\x82\x8D"
        "\xE3\x81\x99\xE3\x81\x88";
    const char kHirosue[] = "\xE5\xBA\x83\xE6\x9C\xAB";

    mock->AddLookupPrefix(kHirosueHiragana, kHirosueHiragana,
                          kHirosue, Token::NONE);

    // "ゆーざー"
    const char kYuzaHiragana[] =
        "\xe3\x82\x86\xe3\x83\xbc\xe3\x81\x96\xe3\x83\xbc";
    // "ユーザー"
    const char kYuza[] = "\xe3\x83\xa6\xe3\x83\xbc\xe3\x82\xb6\xe3\x83\xbc";
    // For dictionary suggestion
    mock->AddLookupPredictive(
        kYuzaHiragana, kYuzaHiragana, kYuza, Token::USER_DICTIONARY);
    // For realtime conversion
    mock->AddLookupPrefix(
        kYuzaHiragana, kYuzaHiragana, kYuza, Token::USER_DICTIONARY);

    // Some English entries
    mock->AddLookupPredictive("conv", "converge", "converge", Token::NONE);
    mock->AddLookupPredictive("conv", "converged", "converged", Token::NONE);
    mock->AddLookupPredictive("conv", "convergent", "convergent", Token::NONE);
    mock->AddLookupPredictive("con", "contraction", "contraction", Token::NONE);
    mock->AddLookupPredictive("con", "control", "control", Token::NONE);
  }

  MockDataAndPredictor *CreateDictionaryPredictorWithMockData() {
    MockDataAndPredictor *ret = new MockDataAndPredictor;
    ret->Init();
    AddWordsToMockDic(ret->mutable_dictionary());
    return ret;
  }

  void GenerateKeyEvents(const string &text, vector<commands::KeyEvent> *keys) {
    keys->clear();

    const char *begin = text.data();
    const char *end = text.data() + text.size();
    size_t mblen = 0;

    while (begin < end) {
      commands::KeyEvent key;
      const char32 w = Util::UTF8ToUCS4(begin, end, &mblen);
      if (Util::GetCharacterSet(w) == Util::ASCII) {
        key.set_key_code(*begin);
      } else {
        key.set_key_code('?');
        key.set_key_string(string(begin, mblen));
      }
      begin += mblen;
      keys->push_back(key);
    }
  }

  void InsertInputSequence(const string &text, composer::Composer *composer) {
    vector<commands::KeyEvent> keys;
    GenerateKeyEvents(text, &keys);

    for (size_t i = 0; i < keys.size(); ++i) {
      composer->InsertCharacterKeyEvent(keys[i]);
    }
  }

  void InsertInputSequenceForProbableKeyEvent(const string &text,
                                              const uint32 *corrected_key_codes,
                                              composer::Composer *composer) {
    vector<commands::KeyEvent> keys;
    GenerateKeyEvents(text, &keys);

    for (size_t i = 0; i < keys.size(); ++i) {
      if (keys[i].key_code() != corrected_key_codes[i]) {
        commands::KeyEvent::ProbableKeyEvent *probable_key_event;

        probable_key_event = keys[i].add_probable_key_event();
        probable_key_event->set_key_code(keys[i].key_code());
        probable_key_event->set_probability(0.9f);

        probable_key_event = keys[i].add_probable_key_event();
        probable_key_event->set_key_code(corrected_key_codes[i]);
        probable_key_event->set_probability(0.1f);
      }
      composer->InsertCharacterKeyEvent(keys[i]);
    }
  }

  void ExpansionForUnigramTestHelper(bool use_expansion) {
    config_->set_use_dictionary_suggest(true);
    config_->set_use_realtime_conversion(false);
    config_->set_use_kana_modifier_insensitive_conversion(use_expansion);

    table_->LoadFromFile("system://romanji-hiragana.tsv");
    composer_->SetTable(table_.get());
    unique_ptr<MockDataAndPredictor> data_and_predictor(
        new MockDataAndPredictor);
    // CallCheckDictionary is managed by data_and_predictor;
    CallCheckDictionary *check_dictionary = new CallCheckDictionary;
    data_and_predictor->Init(check_dictionary, NULL);
    const TestableDictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

    {
      Segments segments;
      segments.set_request_type(Segments::PREDICTION);
      request_->set_kana_modifier_insensitive_conversion(use_expansion);
      InsertInputSequence("gu-g", composer_.get());
      Segment *segment = segments.add_segment();
      CHECK(segment);
      string query;
      composer_->GetQueryForPrediction(&query);
      segment->set_key(query);

      EXPECT_CALL(*check_dictionary,
                  LookupPredictive(_, ::testing::Ref(*convreq_), _));

      vector<TestableDictionaryPredictor::Result> results;
      predictor->AggregateUnigramPrediction(
          TestableDictionaryPredictor::UNIGRAM,
          *convreq_, segments, &results);
    }
  }

  void ExpansionForBigramTestHelper(bool use_expansion) {
    config_->set_use_dictionary_suggest(true);
    config_->set_use_realtime_conversion(false);
    config_->set_use_kana_modifier_insensitive_conversion(use_expansion);

    table_->LoadFromFile("system://romanji-hiragana.tsv");
    composer_->SetTable(table_.get());
    unique_ptr<MockDataAndPredictor> data_and_predictor(
        new MockDataAndPredictor);
    // CallCheckDictionary is managed by data_and_predictor;
    CallCheckDictionary *check_dictionary = new CallCheckDictionary;
    data_and_predictor->Init(check_dictionary, NULL);
    const TestableDictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

    {
      Segments segments;
      segments.set_request_type(Segments::PREDICTION);
      // History segment's key and value should be in the dictionary
      Segment *segment = segments.add_segment();
      CHECK(segment);
      segment->set_segment_type(Segment::HISTORY);
      // "ぐーぐる"
      segment->set_key("\xe3\x81\x90\xe3\x83\xbc\xe3\x81\x90\xe3\x82\x8b");
      Segment::Candidate *cand = segment->add_candidate();
      // "ぐーぐる"
      cand->key = "\xe3\x81\x90\xe3\x83\xbc\xe3\x81\x90\xe3\x82\x8b";
      // "ぐーぐる"
      cand->content_key = "\xe3\x81\x90\xe3\x83\xbc\xe3\x81\x90\xe3\x82\x8b";
      // "グーグル"
      cand->value = "\xe3\x82\xb0\xe3\x83\xbc\xe3\x82\xb0\xe3\x83\xab";
      // "グーグル"
      cand->content_value = "\xe3\x82\xb0\xe3\x83\xbc\xe3\x82\xb0\xe3\x83\xab";

      segment = segments.add_segment();
      CHECK(segment);

      request_->set_kana_modifier_insensitive_conversion(use_expansion);
      InsertInputSequence("m", composer_.get());
      string query;
      composer_->GetQueryForPrediction(&query);
      segment->set_key(query);

      // History key and value should be in the dictionary.
      EXPECT_CALL(*check_dictionary,
                  LookupPrefix(_, ::testing::Ref(*convreq_), _))
          .WillOnce(LookupPrefixOneToken(
              // "ぐーぐる"
              "\xe3\x81\x90\xe3\x83\xbc\xe3\x81\x90\xe3\x82\x8b",
              // "グーグル"
              "\xe3\x82\xb0\xe3\x83\xbc\xe3\x82\xb0\xe3\x83\xab",
              1, 1));
      EXPECT_CALL(*check_dictionary,
                  LookupPredictive(_, ::testing::Ref(*convreq_), _));

      vector<TestableDictionaryPredictor::Result> results;
      predictor->AggregateBigramPrediction(
          TestableDictionaryPredictor::BIGRAM,
          *convreq_, segments, &results);
    }
  }

  void ExpansionForSuffixTestHelper(bool use_expansion) {
    config_->set_use_dictionary_suggest(true);
    config_->set_use_realtime_conversion(false);
    config_->set_use_kana_modifier_insensitive_conversion(use_expansion);

    table_->LoadFromFile("system://romanji-hiragana.tsv");
    composer_->SetTable(table_.get());
    unique_ptr<MockDataAndPredictor> data_and_predictor(
        new MockDataAndPredictor);
    // CallCheckDictionary is managed by data_and_predictor.
    CallCheckDictionary *check_dictionary = new CallCheckDictionary;
    data_and_predictor->Init(NULL, check_dictionary);
    const TestableDictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

    {
      Segments segments;
      segments.set_request_type(Segments::PREDICTION);
      Segment *segment = segments.add_segment();
      CHECK(segment);

      request_->set_kana_modifier_insensitive_conversion(use_expansion);
      InsertInputSequence("des", composer_.get());
      string query;
      composer_->GetQueryForPrediction(&query);
      segment->set_key(query);

      EXPECT_CALL(*check_dictionary,
                  LookupPredictive(_, ::testing::Ref(*convreq_), _));

      vector<TestableDictionaryPredictor::Result> results;
      predictor->AggregateSuffixPrediction(
          TestableDictionaryPredictor::SUFFIX,
          *convreq_, segments, &results);
    }
  }

  bool FindCandidateByValue(
      const Segment &segment,
      const string &value) {
    for (size_t i = 0; i < segment.candidates_size(); ++i) {
      const Segment::Candidate &c = segment.candidate(i);
      if (c.value == value) {
        return true;
      }
    }
    return false;
  }

  bool FindResultByValue(
      const vector<TestableDictionaryPredictor::Result> &results,
      const string &value) {
    for (size_t i = 0; i < results.size(); ++i) {
      if (results[i].value == value) {
        return true;
      }
    }
    return false;
  }

  void AggregateEnglishPredictionTestHelper(
      transliteration::TransliterationType input_mode,
      const char *key, const char *expected_prefix,
      const char *expected_values[], size_t expected_values_size) {
    unique_ptr<MockDataAndPredictor> data_and_predictor(
        CreateDictionaryPredictorWithMockData());
    const TestableDictionaryPredictor *predictor =
        data_and_predictor->dictionary_predictor();

    table_->LoadFromFile("system://romanji-hiragana.tsv");
    composer_->Reset();
    composer_->SetTable(table_.get());
    composer_->SetInputMode(input_mode);
    InsertInputSequence(key, composer_.get());

    Segments segments;
    MakeSegmentsForPrediction(key, &segments);

    vector<TestableDictionaryPredictor::Result> results;
    predictor->AggregateEnglishPrediction(
        TestableDictionaryPredictor::ENGLISH,
        *convreq_, segments, &results);

    set<string> values;
    for (size_t i = 0; i < results.size(); ++i) {
      EXPECT_EQ(TestableDictionaryPredictor::ENGLISH, results[i].types);
      EXPECT_TRUE(Util::StartsWith(results[i].value, expected_prefix))
          << results[i].value
          << " doesn't start with " << expected_prefix;
      values.insert(results[i].value);
    }
    for (size_t i = 0; i < expected_values_size; ++i) {
      EXPECT_TRUE(values.find(expected_values[i]) != values.end())
          << expected_values[i] << " isn't in the results";
    }
  }

  void AggregateTypeCorrectingTestHelper(
      const char *key,
      const uint32 *corrected_key_codes,
      const char *expected_values[],
      size_t expected_values_size) {
    request_->set_special_romanji_table(
        commands::Request::QWERTY_MOBILE_TO_HIRAGANA);

    unique_ptr<MockDataAndPredictor> data_and_predictor(
        CreateDictionaryPredictorWithMockData());
    const TestableDictionaryPredictor *predictor =
        data_and_predictor->dictionary_predictor();

    table_->LoadFromFile("system://qwerty_mobile-hiragana.tsv");
    table_->typing_model_ = Singleton<MockTypingModel>::get();
    InsertInputSequenceForProbableKeyEvent(
        key, corrected_key_codes, composer_.get());

    Segments segments;
    MakeSegmentsForPrediction(key, &segments);

    vector<TestableDictionaryPredictor::Result> results;
    predictor->AggregateTypeCorrectingPrediction(
        TestableDictionaryPredictor::TYPING_CORRECTION,
        *convreq_, segments, &results);

    set<string> values;
    for (size_t i = 0; i < results.size(); ++i) {
      EXPECT_EQ(TestableDictionaryPredictor::TYPING_CORRECTION,
                results[i].types);
      values.insert(results[i].value);
    }
    for (size_t i = 0; i < expected_values_size; ++i) {
      EXPECT_TRUE(values.find(expected_values[i]) != values.end())
          << expected_values[i] << " isn't in the results";
    }
  }

  unique_ptr<composer::Composer> composer_;
  unique_ptr<composer::Table> table_;
  unique_ptr<ConversionRequest> convreq_;
  unique_ptr<config::Config> config_;
  unique_ptr<commands::Request> request_;

 private:
  const bool default_expansion_flag_;
  unique_ptr<ImmutableConverterInterface> immutable_converter_;
  mozc::usage_stats::scoped_usage_stats_enabler usage_stats_enabler_;
};

TEST_F(DictionaryPredictorTest, OnOffTest) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  // turn off
  Segments segments;
  config_->set_use_dictionary_suggest(false);
  config_->set_use_realtime_conversion(false);

  // "ぐーぐるあ"
  MakeSegmentsForSuggestion
      ("\xE3\x81\x90\xE3\x83\xBC\xE3\x81\x90\xE3\x82\x8B\xE3\x81\x82",
       &segments);
  EXPECT_FALSE(predictor->PredictForRequest(*convreq_, &segments));

  // turn on
  // "ぐーぐるあ"
  config_->set_use_dictionary_suggest(true);
  MakeSegmentsForSuggestion
      ("\xE3\x81\x90\xE3\x83\xBC\xE3\x81\x90\xE3\x82\x8B\xE3\x81\x82",
       &segments);
  EXPECT_TRUE(predictor->PredictForRequest(*convreq_, &segments));

  // empty query
  MakeSegmentsForSuggestion("", &segments);
  EXPECT_FALSE(predictor->PredictForRequest(*convreq_, &segments));
}

TEST_F(DictionaryPredictorTest, PartialSuggestion) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  {
    // Set up mock converter.
    Segments segments;
    Segment *segment = segments.add_segment();
    Segment::Candidate *candidate = segment->add_candidate();
    candidate->value = "Realtime top result";
    ConverterMock *converter = data_and_predictor->mutable_converter_mock();
    converter->SetStartConversionForRequest(&segments, true);
  }
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  Segments segments;
  config_->set_use_dictionary_suggest(true);
  config_->set_use_realtime_conversion(true);
  // turn on mobile mode
  request_->set_mixed_conversion(true);

  // "ぐーぐるあ"
  segments.Clear();
  segments.set_max_prediction_candidates_size(10);
  segments.set_request_type(Segments::PARTIAL_SUGGESTION);
  Segment *seg = segments.add_segment();
  seg->set_key("\xE3\x81\x90\xE3\x83\xBC\xE3\x81\x90\xE3\x82\x8B\xE3\x81\x82");
  seg->set_segment_type(Segment::FREE);
  EXPECT_TRUE(predictor->PredictForRequest(*convreq_, &segments));
}

TEST_F(DictionaryPredictorTest, BigramTest) {
  Segments segments;
  config_->set_use_dictionary_suggest(true);

  // "あ"
  MakeSegmentsForSuggestion("\xE3\x81\x82", &segments);

  // history is "グーグル"
  PrependHistorySegments("\xE3\x81\x90\xE3\x83\xBC\xE3\x81\x90\xE3\x82\x8B",
                         "\xE3\x82\xB0\xE3\x83\xBC\xE3\x82\xB0\xE3\x83\xAB",
                         &segments);

  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();
  // "グーグルアドセンス" will be returned.
  EXPECT_TRUE(predictor->PredictForRequest(*convreq_, &segments));
}

TEST_F(DictionaryPredictorTest, BigramTestWithZeroQuery) {
  Segments segments;
  config_->set_use_dictionary_suggest(true);
  request_->set_zero_query_suggestion(true);

  // current query is empty
  MakeSegmentsForSuggestion("", &segments);

  // history is "グーグル"
  PrependHistorySegments("\xE3\x81\x90\xE3\x83\xBC\xE3\x81\x90\xE3\x82\x8B",
                         "\xE3\x82\xB0\xE3\x83\xBC\xE3\x82\xB0\xE3\x83\xAB",
                         &segments);

  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();
  EXPECT_TRUE(predictor->PredictForRequest(*convreq_, &segments));
}

// Check that previous candidate never be shown at the current candidate.
TEST_F(DictionaryPredictorTest, Regression3042706) {
  Segments segments;
  config_->set_use_dictionary_suggest(true);

  // "だい"
  MakeSegmentsForSuggestion("\xE3\x81\xA0\xE3\x81\x84", &segments);

  // history is "きょうと/京都"
  PrependHistorySegments("\xE3\x81\x8D\xE3\x82\x87"
                         "\xE3\x81\x86\xE3\x81\xA8",
                         "\xE4\xBA\xAC\xE9\x83\xBD",
                         &segments);

  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();
  EXPECT_TRUE(predictor->PredictForRequest(*convreq_,
                                           &segments));
  EXPECT_EQ(2, segments.segments_size());   // history + current
  for (int i = 0; i < segments.segment(1).candidates_size(); ++i) {
    const Segment::Candidate &candidate = segments.segment(1).candidate(i);
    // "京都"
    EXPECT_FALSE(Util::StartsWith(candidate.content_value,
                                  "\xE4\xBA\xAC\xE9\x83\xBD"));
    // "だい"
    EXPECT_TRUE(Util::StartsWith(candidate.content_key,
                                 "\xE3\x81\xA0\xE3\x81\x84"));
  }
}

TEST_F(DictionaryPredictorTest, GetPredictionTypes) {
  Segments segments;
  config_->set_use_dictionary_suggest(true);
  config_->set_use_realtime_conversion(false);

  // empty segments
  {
    EXPECT_EQ(
        DictionaryPredictor::NO_PREDICTION,
        DictionaryPredictor::GetPredictionTypes(*convreq_, segments));
  }

  // normal segments
  {
    // "てすとだよ"
    MakeSegmentsForSuggestion("\xE3\x81\xA6\xE3\x81\x99\xE3"
                              "\x81\xA8\xE3\x81\xA0\xE3\x82\x88",
                              &segments);
    EXPECT_EQ(
        DictionaryPredictor::UNIGRAM,
        DictionaryPredictor::GetPredictionTypes(*convreq_, segments));

    segments.set_request_type(Segments::PREDICTION);
    EXPECT_EQ(
        DictionaryPredictor::UNIGRAM,
        DictionaryPredictor::GetPredictionTypes(*convreq_, segments));

    segments.set_request_type(Segments::CONVERSION);
    EXPECT_EQ(
        DictionaryPredictor::NO_PREDICTION,
        DictionaryPredictor::GetPredictionTypes(*convreq_, segments));
  }

  // short key
  {
    // "てす"
    MakeSegmentsForSuggestion("\xE3\x81\xA6\xE3\x81\x99",
                              &segments);
    EXPECT_EQ(
        DictionaryPredictor::NO_PREDICTION,
        DictionaryPredictor::GetPredictionTypes(*convreq_, segments));

    // on prediction mode, return UNIGRAM
    segments.set_request_type(Segments::PREDICTION);
    EXPECT_EQ(
        DictionaryPredictor::UNIGRAM,
        DictionaryPredictor::GetPredictionTypes(*convreq_, segments));
  }

  // zipcode-like key
  {
    MakeSegmentsForSuggestion("0123", &segments);
    EXPECT_EQ(
        DictionaryPredictor::NO_PREDICTION,
        DictionaryPredictor::GetPredictionTypes(*convreq_, segments));
  }

  // History is short => UNIGRAM
  {
    // "てすとだよ"
    MakeSegmentsForSuggestion("\xE3\x81\xA6\xE3\x81\x99\xE3\x81\xA8"
                              "\xE3\x81\xA0\xE3\x82\x88", &segments);
    PrependHistorySegments("A", "A", &segments);
    EXPECT_EQ(
        DictionaryPredictor::UNIGRAM,
        DictionaryPredictor::GetPredictionTypes(*convreq_, segments));
  }

  // both History and current segment are long => UNIGRAM|BIGRAM
  {
    // "てすとだよ"
    MakeSegmentsForSuggestion("\xE3\x81\xA6\xE3\x81\x99\xE3\x81\xA8"
                              "\xE3\x81\xA0\xE3\x82\x88", &segments);
    PrependHistorySegments("\xE3\x81\xA6\xE3\x81\x99\xE3\x81\xA8"
                           "\xE3\x81\xA0\xE3\x82\x88", "abc", &segments);
    EXPECT_EQ(
        DictionaryPredictor::UNIGRAM |
        DictionaryPredictor::BIGRAM,
        DictionaryPredictor::GetPredictionTypes(*convreq_, segments));
  }

  // Current segment is short => BIGRAM
  {
    MakeSegmentsForSuggestion("A", &segments);
    // "てすとだよ"
    PrependHistorySegments("\xE3\x81\xA6\xE3\x81\x99"
                           "\xE3\x81\xA8\xE3\x81\xA0\xE3\x82\x88",
                           "abc", &segments);
    EXPECT_EQ(
        DictionaryPredictor::BIGRAM,
        DictionaryPredictor::GetPredictionTypes(*convreq_, segments));
  }

  // Typing correction type shouldn't be appended.
  {
    // "ｐはよう"
    MakeSegmentsForSuggestion(
        "\xEF\xBD\x90\xE3\x81\xAF\xE3\x82\x88\xE3\x81\x86", &segments);
    EXPECT_FALSE(
        DictionaryPredictor::TYPING_CORRECTION
        & DictionaryPredictor::GetPredictionTypes(*convreq_,
                                                  segments));
  }

  // Input mode is HALF_ASCII or FULL_ASCII => ENGLISH
  {
    config_->set_use_dictionary_suggest(true);

    MakeSegmentsForSuggestion("hel", &segments);

    composer_->SetInputMode(transliteration::HALF_ASCII);
    EXPECT_EQ(
        DictionaryPredictor::ENGLISH,
        DictionaryPredictor::GetPredictionTypes(*convreq_, segments));

    composer_->SetInputMode(transliteration::FULL_ASCII);
    EXPECT_EQ(
        DictionaryPredictor::ENGLISH,
        DictionaryPredictor::GetPredictionTypes(*convreq_, segments));

    // When dictionary suggest is turned off, English prediction should be
    // disabled.
    config_->set_use_dictionary_suggest(false);

    composer_->SetInputMode(transliteration::HALF_ASCII);
    EXPECT_EQ(
        DictionaryPredictor::NO_PREDICTION,
        DictionaryPredictor::GetPredictionTypes(*convreq_, segments));

    composer_->SetInputMode(transliteration::FULL_ASCII);
    EXPECT_EQ(
        DictionaryPredictor::NO_PREDICTION,
        DictionaryPredictor::GetPredictionTypes(*convreq_, segments));

    config_->set_use_dictionary_suggest(true);

    segments.set_request_type(Segments::PARTIAL_SUGGESTION);
    composer_->SetInputMode(transliteration::HALF_ASCII);
    EXPECT_EQ(
        DictionaryPredictor::ENGLISH | DictionaryPredictor::REALTIME,
        DictionaryPredictor::GetPredictionTypes(*convreq_, segments));

    composer_->SetInputMode(transliteration::FULL_ASCII);
    EXPECT_EQ(
        DictionaryPredictor::ENGLISH | DictionaryPredictor::REALTIME,
        DictionaryPredictor::GetPredictionTypes(*convreq_, segments));

    config_->set_use_dictionary_suggest(false);

    composer_->SetInputMode(transliteration::HALF_ASCII);
    EXPECT_EQ(
        DictionaryPredictor::REALTIME,
        DictionaryPredictor::GetPredictionTypes(*convreq_, segments));

    composer_->SetInputMode(transliteration::FULL_ASCII);
    EXPECT_EQ(
        DictionaryPredictor::REALTIME,
        DictionaryPredictor::GetPredictionTypes(*convreq_, segments));
  }
}

TEST_F(DictionaryPredictorTest, GetPredictionTypesTestWithTypingCorrection) {
  Segments segments;
  config_->set_use_dictionary_suggest(true);
  config_->set_use_realtime_conversion(false);
  config_->set_use_typing_correction(true);

  // "ｐはよう"
  MakeSegmentsForSuggestion(
      "\xEF\xBD\x90\xE3\x81\xAF\xE3\x82\x88\xE3\x81\x86", &segments);
  EXPECT_EQ(
      DictionaryPredictor::UNIGRAM | DictionaryPredictor::TYPING_CORRECTION,
      DictionaryPredictor::GetPredictionTypes(*convreq_, segments));
}

TEST_F(DictionaryPredictorTest, GetPredictionTypesTestWithZeroQuerySuggestion) {
  Segments segments;
  config_->set_use_dictionary_suggest(true);
  config_->set_use_realtime_conversion(false);
  request_->set_zero_query_suggestion(true);

  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  // empty segments
  {
    EXPECT_EQ(
        DictionaryPredictor::NO_PREDICTION,
        predictor->GetPredictionTypes(*convreq_, segments));
  }

  // normal segments
  {
    // "てすとだよ"
    MakeSegmentsForSuggestion("\xE3\x81\xA6\xE3\x81\x99\xE3"
                              "\x81\xA8\xE3\x81\xA0\xE3\x82\x88",
                              &segments);
    EXPECT_EQ(
        DictionaryPredictor::UNIGRAM,
        predictor->GetPredictionTypes(*convreq_, segments));

    segments.set_request_type(Segments::PREDICTION);
    EXPECT_EQ(
        DictionaryPredictor::UNIGRAM,
        predictor->GetPredictionTypes(*convreq_, segments));

    segments.set_request_type(Segments::CONVERSION);
    EXPECT_EQ(
        DictionaryPredictor::NO_PREDICTION,
        predictor->GetPredictionTypes(*convreq_, segments));
  }

  // short key
  {
    // "て"
    MakeSegmentsForSuggestion("\xE3\x81\xA6",
                              &segments);
    EXPECT_EQ(
        DictionaryPredictor::UNIGRAM,
        predictor->GetPredictionTypes(*convreq_, segments));

    // on prediction mode, return UNIGRAM
    segments.set_request_type(Segments::PREDICTION);
    EXPECT_EQ(
        DictionaryPredictor::UNIGRAM,
        predictor->GetPredictionTypes(*convreq_, segments));
  }

  // History is short => UNIGRAM
  {
    // "てすとだよ"
    MakeSegmentsForSuggestion("\xE3\x81\xA6\xE3\x81\x99\xE3\x81\xA8"
                              "\xE3\x81\xA0\xE3\x82\x88", &segments);
    PrependHistorySegments("A", "A", &segments);
    EXPECT_EQ(
        DictionaryPredictor::UNIGRAM | DictionaryPredictor::SUFFIX,
        predictor->GetPredictionTypes(*convreq_, segments));
  }

  // both History and current segment are long => UNIGRAM|BIGRAM
  {
    // "てすとだよ"
    MakeSegmentsForSuggestion("\xE3\x81\xA6\xE3\x81\x99\xE3\x81\xA8"
                              "\xE3\x81\xA0\xE3\x82\x88", &segments);
    PrependHistorySegments("\xE3\x81\xA6\xE3\x81\x99\xE3\x81\xA8"
                           "\xE3\x81\xA0\xE3\x82\x88", "abc", &segments);
    EXPECT_EQ(
        DictionaryPredictor::UNIGRAM | DictionaryPredictor::BIGRAM |
        DictionaryPredictor::SUFFIX,
        predictor->GetPredictionTypes(*convreq_, segments));
  }

  {
    MakeSegmentsForSuggestion("A", &segments);
    // "てすとだよ"
    PrependHistorySegments("\xE3\x81\xA6\xE3\x81\x99"
                           "\xE3\x81\xA8\xE3\x81\xA0\xE3\x82\x88",
                           "abc", &segments);
    EXPECT_EQ(
        DictionaryPredictor::BIGRAM | DictionaryPredictor::UNIGRAM |
        DictionaryPredictor::SUFFIX,
        predictor->GetPredictionTypes(*convreq_, segments));
  }

  {
    MakeSegmentsForSuggestion("", &segments);
    // "て"
    PrependHistorySegments("\xE3\x81\xA6", "abc", &segments);
    EXPECT_EQ(
        DictionaryPredictor::SUFFIX,
        predictor->GetPredictionTypes(*convreq_, segments));
  }

  {
    MakeSegmentsForSuggestion("A", &segments);
    // "て"
    PrependHistorySegments("\xE3\x81\xA6", "abc", &segments);
    EXPECT_EQ(
        DictionaryPredictor::UNIGRAM | DictionaryPredictor::SUFFIX,
        predictor->GetPredictionTypes(*convreq_, segments));
  }

  {
    MakeSegmentsForSuggestion("", &segments);
    // "てすとだよ"
    PrependHistorySegments("\xE3\x81\xA6\xE3\x81\x99"
                           "\xE3\x81\xA8\xE3\x81\xA0\xE3\x82\x88",
                           "abc", &segments);
    EXPECT_EQ(
        DictionaryPredictor::BIGRAM | DictionaryPredictor::SUFFIX,
        predictor->GetPredictionTypes(*convreq_, segments));
  }
}

TEST_F(DictionaryPredictorTest, AggregateUnigramPrediction) {
  Segments segments;
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  // "ぐーぐるあ"
  const char kKey[] = "\xE3\x81\x90\xE3\x83\xBC"
      "\xE3\x81\x90\xE3\x82\x8B\xE3\x81\x82";

  MakeSegmentsForSuggestion(kKey, &segments);

  vector<DictionaryPredictor::Result> results;

  predictor->AggregateUnigramPrediction(
      DictionaryPredictor::BIGRAM,
      *convreq_, segments, &results);
  EXPECT_TRUE(results.empty());

  predictor->AggregateUnigramPrediction(
      DictionaryPredictor::REALTIME,
      *convreq_, segments, &results);
  EXPECT_TRUE(results.empty());

  predictor->AggregateUnigramPrediction(
      DictionaryPredictor::UNIGRAM,
      *convreq_, segments, &results);
  EXPECT_FALSE(results.empty());

  for (size_t i = 0; i < results.size(); ++i) {
    EXPECT_EQ(DictionaryPredictor::UNIGRAM, results[i].types);
    EXPECT_TRUE(Util::StartsWith(results[i].key, kKey));
  }

  EXPECT_EQ(1, segments.conversion_segments_size());
}

TEST_F(DictionaryPredictorTest, AggregateBigramPrediction) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  {
    Segments segments;

    // "あ"
    MakeSegmentsForSuggestion("\xE3\x81\x82", &segments);

    // history is "グーグル"
    const char kHistoryKey[] =
        "\xE3\x81\x90\xE3\x83\xBC\xE3\x81\x90\xE3\x82\x8B";
    const char kHistoryValue[] =
        "\xE3\x82\xB0\xE3\x83\xBC\xE3\x82\xB0\xE3\x83\xAB";

    PrependHistorySegments(kHistoryKey, kHistoryValue, &segments);

    vector<DictionaryPredictor::Result> results;

    predictor->AggregateBigramPrediction(
        DictionaryPredictor::UNIGRAM,
        *convreq_, segments, &results);
    EXPECT_TRUE(results.empty());

    predictor->AggregateBigramPrediction(
        DictionaryPredictor::REALTIME,
        *convreq_, segments, &results);
    EXPECT_TRUE(results.empty());

    predictor->AggregateBigramPrediction(
        DictionaryPredictor::BIGRAM,
        *convreq_, segments, &results);
    EXPECT_FALSE(results.empty());

    for (size_t i = 0; i < results.size(); ++i) {
      // "グーグルアドセンス", "グーグル", "アドセンス"
      // are in the dictionary.
      if (results[i].value == "\xE3\x82\xB0\xE3\x83\xBC\xE3\x82\xB0"
          "\xE3\x83\xAB\xE3\x82\xA2\xE3\x83\x89"
          "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xB9") {
        EXPECT_EQ(DictionaryPredictor::BIGRAM, results[i].types);
      } else {
        EXPECT_EQ(DictionaryPredictor::NO_PREDICTION, results[i].types);
      }
      EXPECT_TRUE(Util::StartsWith(results[i].key, kHistoryKey));
      EXPECT_TRUE(Util::StartsWith(results[i].value, kHistoryValue));
      // Not zero query
      EXPECT_FALSE(
          results[i].source_info &
          Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_SUFFIX);
    }

    EXPECT_EQ(1, segments.conversion_segments_size());
  }

  {
    Segments segments;

    // "と"
    MakeSegmentsForSuggestion("\xE3\x81\x82", &segments);

    // "てす"
    const char kHistoryKey[] = "\xE3\x81\xA6\xE3\x81\x99";
    // "テス"
    const char kHistoryValue[] = "\xE3\x83\x86\xE3\x82\xB9";

    PrependHistorySegments(kHistoryKey, kHistoryValue, &segments);

    vector<DictionaryPredictor::Result> results;

    predictor->AggregateBigramPrediction(
        DictionaryPredictor::BIGRAM,
        *convreq_, segments, &results);
    EXPECT_TRUE(results.empty());
  }
}

TEST_F(DictionaryPredictorTest, AggregateZeroQueryBigramPrediction) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();
  commands::RequestForUnitTest::FillMobileRequest(request_.get());

  {
    Segments segments;

    // Zero query
    MakeSegmentsForSuggestion("", &segments);

    // history is "グーグル"
    const char kHistoryKey[] =
        "\xE3\x81\x90\xE3\x83\xBC\xE3\x81\x90\xE3\x82\x8B";
    const char kHistoryValue[] =
        "\xE3\x82\xB0\xE3\x83\xBC\xE3\x82\xB0\xE3\x83\xAB";

    PrependHistorySegments(kHistoryKey, kHistoryValue, &segments);

    vector<DictionaryPredictor::Result> results;

    predictor->AggregateBigramPrediction(
        DictionaryPredictor::UNIGRAM, *convreq_, segments, &results);
    EXPECT_TRUE(results.empty());

    predictor->AggregateBigramPrediction(
        DictionaryPredictor::REALTIME, *convreq_, segments, &results);
    EXPECT_TRUE(results.empty());

    predictor->AggregateBigramPrediction(
        DictionaryPredictor::BIGRAM, *convreq_, segments, &results);
    EXPECT_FALSE(results.empty());

    for (size_t i = 0; i < results.size(); ++i) {
      EXPECT_TRUE(Util::StartsWith(results[i].key, kHistoryKey));
      EXPECT_TRUE(Util::StartsWith(results[i].value, kHistoryValue));
      // Zero query
      EXPECT_FALSE(
          results[i].source_info &
          Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_SUFFIX);
    }
  }
}

TEST_F(DictionaryPredictorTest, GetRealtimeCandidateMaxSize) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();
  Segments segments;

  // GetRealtimeCandidateMaxSize has some heuristics so here we test following
  // conditions.
  // - The result must be equal or less than kMaxSize;
  // - If mixed_conversion is the same, the result of SUGGESTION is
  //        equal or less than PREDICTION.
  // - If mixed_conversion is the same, the result of PARTIAL_SUGGESTION is
  //        equal or less than PARTIAL_PREDICTION.
  // - Partial version has equal or greater than non-partial version.

  const size_t kMaxSize = 100;

  // non-partial, non-mixed-conversion
  segments.set_request_type(Segments::PREDICTION);
  const size_t prediction_no_mixed =
      predictor->GetRealtimeCandidateMaxSize(segments, false, kMaxSize);
  EXPECT_GE(kMaxSize, prediction_no_mixed);

  segments.set_request_type(Segments::SUGGESTION);
  const size_t suggestion_no_mixed =
      predictor->GetRealtimeCandidateMaxSize(segments, false, kMaxSize);
  EXPECT_GE(kMaxSize, suggestion_no_mixed);
  EXPECT_LE(suggestion_no_mixed, prediction_no_mixed);

  // non-partial, mixed-conversion
  segments.set_request_type(Segments::PREDICTION);
  const size_t prediction_mixed =
      predictor->GetRealtimeCandidateMaxSize(segments, true, kMaxSize);
  EXPECT_GE(kMaxSize, prediction_mixed);

  segments.set_request_type(Segments::SUGGESTION);
  const size_t suggestion_mixed =
      predictor->GetRealtimeCandidateMaxSize(segments, true, kMaxSize);
  EXPECT_GE(kMaxSize, suggestion_mixed);

  // partial, non-mixed-conversion
  segments.set_request_type(Segments::PARTIAL_PREDICTION);
  const size_t partial_prediction_no_mixed =
      predictor->GetRealtimeCandidateMaxSize(segments, false, kMaxSize);
  EXPECT_GE(kMaxSize, partial_prediction_no_mixed);

  segments.set_request_type(Segments::PARTIAL_SUGGESTION);
  const size_t partial_suggestion_no_mixed =
      predictor->GetRealtimeCandidateMaxSize(segments, false, kMaxSize);
  EXPECT_GE(kMaxSize, partial_suggestion_no_mixed);
  EXPECT_LE(partial_suggestion_no_mixed, partial_prediction_no_mixed);

  // partial, mixed-conversion
  segments.set_request_type(Segments::PARTIAL_PREDICTION);
  const size_t partial_prediction_mixed =
      predictor->GetRealtimeCandidateMaxSize(segments, true, kMaxSize);
  EXPECT_GE(kMaxSize, partial_prediction_mixed);

  segments.set_request_type(Segments::PARTIAL_SUGGESTION);
  const size_t partial_suggestion_mixed =
      predictor->GetRealtimeCandidateMaxSize(segments, true, kMaxSize);
  EXPECT_GE(kMaxSize, partial_suggestion_mixed);
  EXPECT_LE(partial_suggestion_mixed, partial_prediction_mixed);

  EXPECT_GE(partial_prediction_no_mixed, prediction_no_mixed);
  EXPECT_GE(partial_prediction_mixed, prediction_mixed);
  EXPECT_GE(partial_suggestion_no_mixed, suggestion_no_mixed);
  EXPECT_GE(partial_suggestion_mixed, suggestion_mixed);
}

TEST_F(DictionaryPredictorTest, GetRealtimeCandidateMaxSizeForMixed) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();
  Segments segments;
  Segment *segment = segments.add_segment();

  const size_t kMaxSize = 100;

  // for short key, try to provide many results as possible
  segment->set_key("short");
  segments.set_request_type(Segments::SUGGESTION);
  const size_t short_suggestion_mixed =
      predictor->GetRealtimeCandidateMaxSize(segments, true, kMaxSize);
  EXPECT_GE(kMaxSize, short_suggestion_mixed);

  segments.set_request_type(Segments::PREDICTION);
  const size_t short_prediction_mixed =
      predictor->GetRealtimeCandidateMaxSize(segments, true, kMaxSize);
  EXPECT_GE(kMaxSize, short_prediction_mixed);

  // for long key, provide few results
  segment->set_key("long_request_key");
  segments.set_request_type(Segments::SUGGESTION);
  const size_t long_suggestion_mixed =
      predictor->GetRealtimeCandidateMaxSize(segments, true, kMaxSize);
  EXPECT_GE(kMaxSize, long_suggestion_mixed);
  EXPECT_GT(short_suggestion_mixed, long_suggestion_mixed);

  segments.set_request_type(Segments::PREDICTION);
  const size_t long_prediction_mixed =
      predictor->GetRealtimeCandidateMaxSize(segments, true, kMaxSize);
  EXPECT_GE(kMaxSize, long_prediction_mixed);
  EXPECT_GT(kMaxSize, long_prediction_mixed + long_suggestion_mixed);
  EXPECT_GT(short_prediction_mixed, long_prediction_mixed);
}

TEST_F(DictionaryPredictorTest, AggregateRealtimeConversion) {
  testing::MockDataManager data_manager;
  unique_ptr<const DictionaryInterface> dictionary(new DictionaryMock);
  unique_ptr<ConverterMock> converter(new ConverterMock);
  unique_ptr<ImmutableConverterInterface> immutable_converter(
      new ImmutableConverterMock);
  unique_ptr<const DictionaryInterface> suffix_dictionary(
      CreateSuffixDictionaryFromDataManager(data_manager));
  unique_ptr<const Connector> connector(
      Connector::CreateFromDataManager(data_manager));
  unique_ptr<const Segmenter> segmenter(
      Segmenter::CreateFromDataManager(data_manager));
  unique_ptr<const SuggestionFilter> suggestion_filter(
      CreateSuggestionFilter(data_manager));
  unique_ptr<TestableDictionaryPredictor> predictor(
      new TestableDictionaryPredictor(converter.get(),
                                      immutable_converter.get(),
                                      dictionary.get(),
                                      suffix_dictionary.get(),
                                      connector.get(),
                                      segmenter.get(),
                                      data_manager.GetPOSMatcher(),
                                      suggestion_filter.get()));

  // "わたしのなまえはなかのです"
  const char kKey[] =
      "\xE3\x82\x8F\xE3\x81\x9F\xE3\x81\x97"
      "\xE3\x81\xAE\xE3\x81\xAA\xE3\x81\xBE"
      "\xE3\x81\x88\xE3\x81\xAF\xE3\x81\xAA"
      "\xE3\x81\x8B\xE3\x81\xAE\xE3\x81\xA7\xE3\x81\x99";

  // Set up mock converter
  {
    // Make segments like:
    // "わたしの"    | "なまえは" | "なかのです"
    // "Watashino" | "Namaeha" | "Nakanodesu"
    Segments segments;

    // ("わたしの", "Watashino")
    Segment *segment = segments.add_segment();
    segment->set_key("\xE3\x82\x8F\xE3\x81\x9F\xE3\x81\x97\xE3\x81\xAE");
    segment->add_candidate()->value = "Watashino";

    // ("なまえは", "Namaeha")
    segment = segments.add_segment();
    segment->set_key("\xE3\x81\xAA\xE3\x81\xBE\xE3\x81\x88\xE3\x81\xAF");
    segment->add_candidate()->value = "Namaeha";

    // ("なかのです", "Nakanodesu")
    segment = segments.add_segment();
    segment->set_key(
        "\xE3\x81\xAA\xE3\x81\x8B\xE3\x81\xAE\xE3\x81\xA7\xE3\x81\x99");
    segment->add_candidate()->value = "Nakanodesu";

    converter->SetStartConversionForRequest(&segments, true);
  }

  // A test case with use_actual_converter_for_realtime_conversion being false,
  // i.e., realtime conversion result is generated by ImmutableConverterMock.
  {
    Segments segments;

    MakeSegmentsForSuggestion(kKey, &segments);

    vector<TestableDictionaryPredictor::Result> results;
    convreq_->set_use_actual_converter_for_realtime_conversion(false);

    predictor->AggregateRealtimeConversion(
        TestableDictionaryPredictor::UNIGRAM, *convreq_, &segments, &results);
    EXPECT_TRUE(results.empty());

    predictor->AggregateRealtimeConversion(
        TestableDictionaryPredictor::BIGRAM, *convreq_, &segments, &results);
    EXPECT_TRUE(results.empty());

    predictor->AggregateRealtimeConversion(
        TestableDictionaryPredictor::REALTIME, *convreq_, &segments, &results);

    ASSERT_EQ(1, results.size());
    EXPECT_EQ(TestableDictionaryPredictor::REALTIME, results[0].types);
    EXPECT_EQ(kKey, results[0].key);
    EXPECT_EQ(3, results[0].inner_segment_boundary.size());
  }

  // A test case with use_actual_converter_for_realtime_conversion being true,
  // i.e., realtime conversion result is generated by ConverterMock.
  {
    Segments segments;

    MakeSegmentsForSuggestion(kKey, &segments);

    vector<TestableDictionaryPredictor::Result> results;
    convreq_->set_use_actual_converter_for_realtime_conversion(true);

    predictor->AggregateRealtimeConversion(
        TestableDictionaryPredictor::UNIGRAM, *convreq_, &segments, &results);
    EXPECT_TRUE(results.empty());

    predictor->AggregateRealtimeConversion(
        TestableDictionaryPredictor::BIGRAM, *convreq_, &segments, &results);
    EXPECT_TRUE(results.empty());

    predictor->AggregateRealtimeConversion(
        TestableDictionaryPredictor::REALTIME, *convreq_, &segments, &results);

    // When |request.use_actual_converter_for_realtime_conversion| is true, the
    // extra label REALTIME_TOP is expected to be added.
    ASSERT_EQ(2, results.size());
    bool realtime_top_found = false;
    for (size_t i = 0; i < results.size(); ++i) {
      EXPECT_EQ(TestableDictionaryPredictor::REALTIME |
                TestableDictionaryPredictor::REALTIME_TOP, results[i].types);
      if (results[i].key == kKey &&
          results[i].value == "WatashinoNamaehaNakanodesu" &&
          results[i].inner_segment_boundary.size() == 3) {
        realtime_top_found = true;
        break;
      }
    }
    EXPECT_TRUE(realtime_top_found);
  }
}

namespace {

struct SimpleSuffixToken {
  const char *key;
  const char *value;
};

const SimpleSuffixToken kSuffixTokens[] = {
  //  { "いか",   "以下" }
  { "\xE3\x81\x84\xE3\x81\x8B",   "\xE4\xBB\xA5\xE4\xB8\x8B" }
};

class TestSuffixDictionary : public DictionaryInterface {
 public:
  TestSuffixDictionary() {}
  virtual ~TestSuffixDictionary() {}

  virtual bool HasKey(StringPiece value) const {
    return false;
  }

  virtual bool HasValue(StringPiece value) const {
    return false;
  }

  virtual void LookupPredictive(StringPiece key,
                                const ConversionRequest &conversion_request,
                                Callback *callback) const {
    Token token;
    for (size_t i = 0; i < arraysize(kSuffixTokens); ++i) {
      const SimpleSuffixToken &suffix_token = kSuffixTokens[i];
      if (!key.empty() && !Util::StartsWith(suffix_token.key, key)) {
        continue;
      }
      switch (callback->OnKey(suffix_token.key)) {
        case Callback::TRAVERSE_DONE:
          return;
        case Callback::TRAVERSE_NEXT_KEY:
          continue;
        case Callback::TRAVERSE_CULL:
          LOG(FATAL) << "Culling is not supported.";
          break;
        default:
          break;
      }
      token.key = suffix_token.key;
      token.value = suffix_token.value;
      token.cost = 1000;
      token.lid = token.rid = 0;
      if (callback->OnToken(token.key, token.key, token) ==
          Callback::TRAVERSE_DONE) {
        break;
      }
    }
  }

  virtual void LookupPrefix(StringPiece key,
                            const ConversionRequest &conversion_request,
                            Callback *callback) const {}

  virtual void LookupExact(StringPiece key,
                           const ConversionRequest &conversion_request,
                           Callback *callback) const {}

  virtual void LookupReverse(StringPiece str,
                             const ConversionRequest &conversion_request,
                             Callback *callback) const {}
};

}  // namespace

TEST_F(DictionaryPredictorTest, GetCandidateCutoffThreshold) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();
  Segments segments;

  segments.set_request_type(Segments::PREDICTION);
  const size_t prediction =
      predictor->GetCandidateCutoffThreshold(segments);

  segments.set_request_type(Segments::SUGGESTION);
  const size_t suggestion =
      predictor->GetCandidateCutoffThreshold(segments);
  EXPECT_LE(suggestion, prediction);
}

TEST_F(DictionaryPredictorTest, AggregateSuffixPrediction) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      new MockDataAndPredictor);
  data_and_predictor->Init(NULL, new TestSuffixDictionary());

  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  Segments segments;

  // "あ"
  MakeSegmentsForSuggestion("\xE3\x81\x82", &segments);

  // history is "グーグル"
  const char kHistoryKey[] =
      "\xE3\x81\x90\xE3\x83\xBC\xE3\x81\x90\xE3\x82\x8B";
  const char kHistoryValue[] =
      "\xE3\x82\xB0\xE3\x83\xBC\xE3\x82\xB0\xE3\x83\xAB";

  PrependHistorySegments(kHistoryKey, kHistoryValue, &segments);

  vector<DictionaryPredictor::Result> results;

  // Since SuffixDictionary only returns when key is "い".
  // result should be empty.
  predictor->AggregateSuffixPrediction(
      DictionaryPredictor::SUFFIX,
      *convreq_, segments, &results);
  EXPECT_TRUE(results.empty());

  results.clear();
  segments.mutable_conversion_segment(0)->set_key("");
  predictor->AggregateSuffixPrediction(
      DictionaryPredictor::SUFFIX,
      *convreq_, segments, &results);
  EXPECT_FALSE(results.empty());

  results.clear();
  predictor->AggregateSuffixPrediction(
      DictionaryPredictor::UNIGRAM,
      *convreq_, segments, &results);
  EXPECT_TRUE(results.empty());

  predictor->AggregateSuffixPrediction(
      DictionaryPredictor::REALTIME,
      *convreq_, segments, &results);
  EXPECT_TRUE(results.empty());

  predictor->AggregateSuffixPrediction(
      DictionaryPredictor::BIGRAM,
      *convreq_, segments, &results);
  EXPECT_TRUE(results.empty());

  // Candidates generated by AggregateSuffixPrediction should have SUFFIX type.
  results.clear();
  // "い"
  segments.mutable_conversion_segment(0)->set_key("\xe3\x81\x84");
  predictor->AggregateSuffixPrediction(
      DictionaryPredictor::SUFFIX | DictionaryPredictor::BIGRAM,
      *convreq_, segments, &results);
  EXPECT_FALSE(results.empty());
  for (size_t i = 0; i < results.size(); ++i) {
    EXPECT_EQ(DictionaryPredictor::SUFFIX, results[i].types);
    // Not zero query
    EXPECT_FALSE(Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_SUFFIX &
                 results[i].source_info);
  }
}

TEST_F(DictionaryPredictorTest, AggregateZeroQuerySuffixPrediction) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      new MockDataAndPredictor);
  data_and_predictor->Init(NULL, new TestSuffixDictionary());

  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  commands::RequestForUnitTest::FillMobileRequest(request_.get());
  Segments segments;

  // Zero query
  MakeSegmentsForSuggestion("", &segments);

  // history is "グーグル"
  const char kHistoryKey[] =
      "\xE3\x81\x90\xE3\x83\xBC\xE3\x81\x90\xE3\x82\x8B";
  const char kHistoryValue[] =
      "\xE3\x82\xB0\xE3\x83\xBC\xE3\x82\xB0\xE3\x83\xAB";

  PrependHistorySegments(kHistoryKey, kHistoryValue, &segments);

  vector<DictionaryPredictor::Result> results;

  // Candidates generated by AggregateSuffixPrediction should have SUFFIX type.
  predictor->AggregateSuffixPrediction(
      DictionaryPredictor::SUFFIX, *convreq_, segments, &results);
  EXPECT_FALSE(results.empty());
  for (size_t i = 0; i < results.size(); ++i) {
    EXPECT_EQ(DictionaryPredictor::SUFFIX, results[i].types);
    // Zero query
    EXPECT_TRUE(Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_SUFFIX &
                results[i].source_info);
  }
}

TEST_F(DictionaryPredictorTest, AggregateEnglishPrediction) {
  // Input mode: HALF_ASCII, Key: lower case
  //   => Prediction should be in half-width lower case.
  {
    const char *kExpectedValues[] = {
      "converge", "converged", "convergent",
    };
    AggregateEnglishPredictionTestHelper(
        transliteration::HALF_ASCII, "conv", "conv",
        kExpectedValues, arraysize(kExpectedValues));
  }
  // Input mode: HALF_ASCII, Key: upper case
  //   => Prediction should be in half-width upper case.
  {
    const char *kExpectedValues[] = {
      "CONVERGE", "CONVERGED", "CONVERGENT",
    };
    AggregateEnglishPredictionTestHelper(
        transliteration::HALF_ASCII, "CONV", "CONV",
        kExpectedValues, arraysize(kExpectedValues));
  }
  // Input mode: HALF_ASCII, Key: capitalized
  //   => Prediction should be half-width and capitalized
  {
    const char *kExpectedValues[] = {
      "Converge", "Converged", "Convergent",
    };
    AggregateEnglishPredictionTestHelper(
        transliteration::HALF_ASCII, "Conv", "Conv",
        kExpectedValues, arraysize(kExpectedValues));
  }
  // Input mode: FULL_ASCII, Key: lower case
  //   => Prediction should be in full-wdith lower case.
  {
    const char *kExpectedValues[] = {
      // "ｃｏｎｖｅｒｇｅ"
      "\xEF\xBD\x83\xEF\xBD\x8F\xEF\xBD\x8E\xEF\xBD\x96\xEF\xBD\x85"
      "\xEF\xBD\x92\xEF\xBD\x87\xEF\xBD\x85",
      // "ｃｏｎｖｅｒｇｅｄ"
      "\xEF\xBD\x83\xEF\xBD\x8F\xEF\xBD\x8E\xEF\xBD\x96\xEF\xBD\x85"
      "\xEF\xBD\x92\xEF\xBD\x87\xEF\xBD\x85\xEF\xBD\x84",
      // "ｃｏｎｖｅｒｇｅｎｔ"
      "\xEF\xBD\x83\xEF\xBD\x8F\xEF\xBD\x8E\xEF\xBD\x96\xEF\xBD\x85"
      "\xEF\xBD\x92\xEF\xBD\x87\xEF\xBD\x85\xEF\xBD\x8E\xEF\xBD\x94",
    };
    AggregateEnglishPredictionTestHelper(
        transliteration::FULL_ASCII,
        "conv",
        "\xEF\xBD\x83\xEF\xBD\x8F\xEF\xBD\x8E\xEF\xBD\x96",  // "ｃｏｎｖ"
        kExpectedValues, arraysize(kExpectedValues));
  }
  // Input mode: FULL_ASCII, Key: upper case
  //   => Prediction should be in full-width upper case.
  {
    const char *kExpectedValues[] = {
      // "ＣＯＮＶＥＲＧＥ"
      "\xEF\xBC\xA3\xEF\xBC\xAF\xEF\xBC\xAE\xEF\xBC\xB6\xEF\xBC\xA5"
      "\xEF\xBC\xB2\xEF\xBC\xA7\xEF\xBC\xA5",
      // "ＣＯＮＶＥＲＧＥＤ"
      "\xEF\xBC\xA3\xEF\xBC\xAF\xEF\xBC\xAE\xEF\xBC\xB6\xEF\xBC\xA5"
      "\xEF\xBC\xB2\xEF\xBC\xA7\xEF\xBC\xA5\xEF\xBC\xA4",
      // "ＣＯＮＶＥＲＧＥＮＴ"
      "\xEF\xBC\xA3\xEF\xBC\xAF\xEF\xBC\xAE\xEF\xBC\xB6\xEF\xBC\xA5"
      "\xEF\xBC\xB2\xEF\xBC\xA7\xEF\xBC\xA5\xEF\xBC\xAE\xEF\xBC\xB4",
    };
    AggregateEnglishPredictionTestHelper(
        transliteration::FULL_ASCII,
        "CONV",
        "\xEF\xBC\xA3\xEF\xBC\xAF\xEF\xBC\xAE\xEF\xBC\xB6",  // "ＣＯＮＶ"
        kExpectedValues, arraysize(kExpectedValues));
  }
  // Input mode: FULL_ASCII, Key: capitalized
  //   => Prediction should be full-width and capitalized
  {
    const char *kExpectedValues[] = {
      // "Ｃｏｎｖｅｒｇｅ"
      "\xEF\xBC\xA3\xEF\xBD\x8F\xEF\xBD\x8E\xEF\xBD\x96\xEF\xBD\x85"
      "\xEF\xBD\x92\xEF\xBD\x87\xEF\xBD\x85",
      // "Ｃｏｎｖｅｒｇｅｄ"
      "\xEF\xBC\xA3\xEF\xBD\x8F\xEF\xBD\x8E\xEF\xBD\x96\xEF\xBD\x85"
      "\xEF\xBD\x92\xEF\xBD\x87\xEF\xBD\x85\xEF\xBD\x84",
      // "Ｃｏｎｖｅｒｇｅｎｔ"
      "\xEF\xBC\xA3\xEF\xBD\x8F\xEF\xBD\x8E\xEF\xBD\x96\xEF\xBD\x85"
      "\xEF\xBD\x92\xEF\xBD\x87\xEF\xBD\x85\xEF\xBD\x8E\xEF\xBD\x94",
    };
    AggregateEnglishPredictionTestHelper(
        transliteration::FULL_ASCII,
        "Conv",
        "\xEF\xBC\xA3\xEF\xBD\x8F\xEF\xBD\x8E\xEF\xBD\x96",  // "Ｃｏｎｖ"
        kExpectedValues, arraysize(kExpectedValues));
  }
}

TEST_F(DictionaryPredictorTest, AggregateTypeCorrectingPrediction) {
  config_->set_use_typing_correction(true);

  const char kInputText[] = "gu-huru";
  const uint32 kCorrectedKeyCodes[] = {'g', 'u', '-', 'g', 'u', 'r', 'u'};
  const char *kExpectedValues[] = {
    // "グーグルアドセンス"
    "\xE3\x82\xB0\xE3\x83\xBC\xE3\x82\xB0"
    "\xE3\x83\xAB\xE3\x82\xA2\xE3\x83\x89"
    "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xB9",
    // "グーグルアドワーズ"
    "\xE3\x82\xB0\xE3\x83\xBC\xE3\x82\xB0\xE3\x83\xAB"
    "\xE3\x82\xA2\xE3\x83\x89\xE3\x83\xAF\xE3\x83\xBC\xE3\x82\xBA",
  };
  AggregateTypeCorrectingTestHelper(kInputText,
                                    kCorrectedKeyCodes,
                                    kExpectedValues,
                                    arraysize(kExpectedValues));
}

TEST_F(DictionaryPredictorTest, ZeroQuerySuggestionAfterNumbers) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();
  const POSMatcher &pos_matcher = data_and_predictor->pos_matcher();
  Segments segments;

  {
    MakeSegmentsForSuggestion("", &segments);

    const char kHistoryKey[] = "12";
    const char kHistoryValue[] = "12";
    const char kExpectedValue[] = "\xE6\x9C\x88";  // "月" (month)
    PrependHistorySegments(kHistoryKey, kHistoryValue, &segments);
    vector<DictionaryPredictor::Result> results;
    predictor->AggregateSuffixPrediction(
        DictionaryPredictor::SUFFIX,
        *convreq_, segments, &results);
    EXPECT_FALSE(results.empty());

    vector<DictionaryPredictor::Result>::const_iterator target =
        results.end();
    for (vector<DictionaryPredictor::Result>::const_iterator it =
             results.begin();
         it != results.end(); ++it) {
      EXPECT_EQ(it->types, DictionaryPredictor::SUFFIX);

      EXPECT_TRUE(
          Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_NUMBER_SUFFIX &
          it->source_info);

      if (it->value == kExpectedValue) {
        target = it;
        break;
      }
    }
    EXPECT_NE(results.end(), target);
    EXPECT_EQ(target->value, kExpectedValue);
    EXPECT_EQ(target->lid, pos_matcher.GetCounterSuffixWordId());
    EXPECT_EQ(target->rid, pos_matcher.GetCounterSuffixWordId());

    // Make sure number suffixes are not suggested when there is a key
    results.clear();
    MakeSegmentsForSuggestion("\xE3\x81\x82", &segments);  // "あ"
    PrependHistorySegments(kHistoryKey, kHistoryValue, &segments);
    predictor->AggregateSuffixPrediction(
        DictionaryPredictor::SUFFIX,
        *convreq_, segments, &results);
    target = results.end();
    for (vector<DictionaryPredictor::Result>::const_iterator it =
             results.begin();
         it != results.end(); ++it) {
      EXPECT_EQ(it->types, DictionaryPredictor::SUFFIX);
      if (it->value == kExpectedValue) {
        target = it;
        break;
      }
    }
    EXPECT_EQ(results.end(), target);
  }

  {
    MakeSegmentsForSuggestion("", &segments);

    const char kHistoryKey[] = "66050713";  // A random number
    const char kHistoryValue[] = "66050713";
    const char kExpectedValue[] = "\xE5\x80\x8B";  // "個" (piece)
    PrependHistorySegments(kHistoryKey, kHistoryValue, &segments);
    vector<DictionaryPredictor::Result> results;
    predictor->AggregateSuffixPrediction(
        DictionaryPredictor::SUFFIX,
        *convreq_, segments, &results);
    EXPECT_FALSE(results.empty());

    bool found = false;
    for (vector<DictionaryPredictor::Result>::const_iterator it =
             results.begin();
         it != results.end(); ++it) {
      EXPECT_EQ(it->types, DictionaryPredictor::SUFFIX);
      if (it->value == kExpectedValue) {
        EXPECT_TRUE(
            Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_NUMBER_SUFFIX &
            it->source_info);
        found = true;
        break;
      }
    }
    EXPECT_TRUE(found);
  }
}

TEST_F(DictionaryPredictorTest, TriggerNumberZeroQuerySuggestion) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();
  const POSMatcher &pos_matcher = data_and_predictor->pos_matcher();

  const struct TestCase {
    const char *history_key;
    const char *history_value;
    const char *find_suffix_value;
    bool expected_result;
  } kTestCases[] = {
    // "月"
    { "12", "12", "\xe6\x9c\x88", true },
    // "１２", "月"
    { "12", "\xef\xbc\x91\xef\xbc\x92", "\xe6\x9c\x88", true },
    // "壱拾弐", "月"
    { "12", "\xe5\xa3\xb1\xe6\x8b\xbe\xe5\xbc\x90", "\xe6\x9c\x88", false },
    // "十二", "月"
    { "12", "\xe5\x8d\x81\xe4\xba\x8c", "\xe6\x9c\x88", false },
    // "一二", "月"
    { "12", "\xe4\xb8\x80\xe4\xba\x8c", "\xe6\x9c\x88", false },
    // "Ⅻ", "月"
    { "12", "\xe2\x85\xab", "\xe6\x9c\x88", false },
    // "あか", "月"
    { "\xe3\x81\x82\xe3\x81\x8b", "12", "\xe6\x9c\x88", true },  // T13N
    // "あか", "１２", "月"
    { "\xe3\x81\x82\xe3\x81\x8b", "\xef\xbc\x91\xef\xbc\x92",
      "\xe6\x9c\x88", true },  // T13N
    // "じゅう", "時"
    { "\xe3\x81\x98\xe3\x82\x85\xe3\x81\x86", "10", "\xe6\x99\x82", true },
    // "じゅう", "１０", "時"
    { "\xe3\x81\x98\xe3\x82\x85\xe3\x81\x86", "\xef\xbc\x91\xef\xbc\x90",
      "\xe6\x99\x82", true },
    // "じゅう", "十", "時"
    { "\xe3\x81\x98\xe3\x82\x85\xe3\x81\x86", "\xe5\x8d\x81",
      "\xe6\x99\x82", false },
    // "じゅう", "拾", "時"
    { "\xe3\x81\x98\xe3\x82\x85\xe3\x81\x86", "\xe6\x8b\xbe",
      "\xe6\x99\x82", false },
  };

  for (size_t i = 0; i < arraysize(kTestCases); ++i) {
    Segments segments;
    MakeSegmentsForSuggestion("", &segments);

    const TestCase &test_case = kTestCases[i];
    PrependHistorySegments(
        test_case.history_key, test_case.history_value, &segments);
    vector<DictionaryPredictor::Result> results;
    predictor->AggregateSuffixPrediction(
        DictionaryPredictor::SUFFIX,
        *convreq_, segments, &results);
    EXPECT_FALSE(results.empty());

    bool found = false;
    for (vector<DictionaryPredictor::Result>::const_iterator it =
             results.begin();
         it != results.end(); ++it) {
      EXPECT_EQ(it->types, DictionaryPredictor::SUFFIX);
      if (it->value == test_case.find_suffix_value &&
          it->lid == pos_matcher.GetCounterSuffixWordId()) {
        EXPECT_TRUE(
          Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_NUMBER_SUFFIX &
          it->source_info);
        found = true;
        break;
      }
    }
    EXPECT_EQ(test_case.expected_result, found) << test_case.history_value;
  }
}

TEST_F(DictionaryPredictorTest, TriggerZeroQuerySuggestion) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  const struct TestCase {
    const char *history_key;
    const char *history_value;
    const char *find_value;
    bool expected_result;
  } kTestCases[] = {
    { "@", "@",
      "gmail.com", true },
    { "!", "!",
      "?", false },
  };

  for (size_t i = 0; i < arraysize(kTestCases); ++i) {
    Segments segments;
    MakeSegmentsForSuggestion("", &segments);

    const TestCase &test_case = kTestCases[i];
    PrependHistorySegments(
        test_case.history_key, test_case.history_value, &segments);
    vector<DictionaryPredictor::Result> results;
    predictor->AggregateSuffixPrediction(
        DictionaryPredictor::SUFFIX,
        *convreq_, segments, &results);
    EXPECT_FALSE(results.empty());

    bool found = false;
    for (vector<DictionaryPredictor::Result>::const_iterator it =
             results.begin();
         it != results.end(); ++it) {
      EXPECT_EQ(it->types, DictionaryPredictor::SUFFIX);
      if (it->value == test_case.find_value &&
          it->lid == 0 /* EOS */) {
        found = true;
        break;
      }
    }
    EXPECT_EQ(test_case.expected_result, found) << test_case.history_value;
  }
}

TEST_F(DictionaryPredictorTest, GetHistoryKeyAndValue) {
  Segments segments;
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  MakeSegmentsForSuggestion("test", &segments);

  string key, value;
  EXPECT_FALSE(predictor->GetHistoryKeyAndValue(segments, &key, &value));

  PrependHistorySegments("key", "value", &segments);
  EXPECT_TRUE(predictor->GetHistoryKeyAndValue(segments, &key, &value));
  EXPECT_EQ("key", key);
  EXPECT_EQ("value", value);
}

TEST_F(DictionaryPredictorTest, IsZipCodeRequest) {
  EXPECT_FALSE(DictionaryPredictor::IsZipCodeRequest(""));
  EXPECT_TRUE(DictionaryPredictor::IsZipCodeRequest("000"));
  EXPECT_TRUE(DictionaryPredictor::IsZipCodeRequest("000"));
  EXPECT_FALSE(DictionaryPredictor::IsZipCodeRequest("ABC"));
  EXPECT_TRUE(DictionaryPredictor::IsZipCodeRequest("---"));
  EXPECT_TRUE(DictionaryPredictor::IsZipCodeRequest("0124-"));
  EXPECT_TRUE(DictionaryPredictor::IsZipCodeRequest("0124-0"));
  EXPECT_TRUE(DictionaryPredictor::IsZipCodeRequest("012-0"));
  EXPECT_TRUE(DictionaryPredictor::IsZipCodeRequest("012-3456"));
  // "０１２-０"
  EXPECT_FALSE(DictionaryPredictor::IsZipCodeRequest(
      "\xef\xbc\x90\xef\xbc\x91\xef\xbc\x92\x2d\xef\xbc\x90"));
}

TEST_F(DictionaryPredictorTest, IsAggressiveSuggestion) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  // "ただしい",
  // "ただしいけめんにかぎる",
  EXPECT_TRUE(predictor->IsAggressiveSuggestion(
      4,      // query_len
      11,     // key_len
      6000,   // cost
      true,   // is_suggestion
      20));   // total_candidates_size

  // cost <= 4000
  EXPECT_FALSE(predictor->IsAggressiveSuggestion(
      4,
      11,
      4000,
      true,
      20));

  // not suggestion
  EXPECT_FALSE(predictor->IsAggressiveSuggestion(
      4,
      11,
      4000,
      false,
      20));

  // total_candidates_size is small
  EXPECT_FALSE(predictor->IsAggressiveSuggestion(
      4,
      11,
      4000,
      true,
      5));

  // query_length = 5
  EXPECT_FALSE(predictor->IsAggressiveSuggestion(
      5,
      11,
      6000,
      true,
      20));

  // "それでも",
  // "それでもぼくはやっていない",
  EXPECT_TRUE(predictor->IsAggressiveSuggestion(
      4,
      13,
      6000,
      true,
      20));

  // cost <= 4000
  EXPECT_FALSE(predictor->IsAggressiveSuggestion(
      4,
      13,
      4000,
      true,
      20));
}

TEST_F(DictionaryPredictorTest, RealtimeConversionStartingWithAlphabets) {
  Segments segments;
  // turn on real-time conversion
  config_->set_use_dictionary_suggest(false);
  config_->set_use_realtime_conversion(true);

  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  // "PCてすと"
  const char kKey[] = "PC\xE3\x81\xA6\xE3\x81\x99\xE3\x81\xA8";
  // "PCテスト"
  const char *kExpectedSuggestionValues[] = {
    "Realtime top result",
    "PC\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88",
  };

  // Set up mock converter for realtime top result.
  {
    Segments segments;
    Segment *segment = segments.add_segment();
    segment->set_key(kKey);
    Segment::Candidate *candidate = segment->add_candidate();
    candidate->value = kExpectedSuggestionValues[0];
    ConverterMock *converter = data_and_predictor->mutable_converter_mock();
    converter->SetStartConversionForRequest(&segments, true);
  }

  MakeSegmentsForSuggestion(kKey, &segments);

  vector<DictionaryPredictor::Result> results;

  convreq_->set_use_actual_converter_for_realtime_conversion(false);
  predictor->AggregateRealtimeConversion(
      DictionaryPredictor::REALTIME, *convreq_, &segments, &results);
  ASSERT_EQ(1, results.size());

  EXPECT_EQ(DictionaryPredictor::REALTIME, results[0].types);
  EXPECT_EQ(kExpectedSuggestionValues[1], results[0].value);
  EXPECT_EQ(1, segments.conversion_segments_size());
}

TEST_F(DictionaryPredictorTest, RealtimeConversionWithSpellingCorrection) {
  Segments segments;
  // turn on real-time conversion
  config_->set_use_dictionary_suggest(false);
  config_->set_use_realtime_conversion(true);

  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  // "かぷりちょうざ"
  const char kCapriHiragana[] = "\xE3\x81\x8B\xE3\x81\xB7\xE3\x82\x8A"
      "\xE3\x81\xA1\xE3\x82\x87\xE3\x81\x86\xE3\x81\x96";

  // Set up mock converter for realtime top result.
  {
    Segments segments;
    Segment *segment = segments.add_segment();
    segment->set_key(kCapriHiragana);
    Segment::Candidate *candidate = segment->add_candidate();
    candidate->value = "Dummy";
    ConverterMock *converter = data_and_predictor->mutable_converter_mock();
    converter->SetStartConversionForRequest(&segments, true);
  }

  MakeSegmentsForSuggestion(kCapriHiragana, &segments);

  vector<DictionaryPredictor::Result> results;

  convreq_->set_use_actual_converter_for_realtime_conversion(false);
  predictor->AggregateUnigramPrediction(
      DictionaryPredictor::UNIGRAM,
      *convreq_, segments, &results);
  ASSERT_FALSE(results.empty());
  EXPECT_NE(0, (results[0].candidate_attributes &
                Segment::Candidate::SPELLING_CORRECTION));

  results.clear();

  // "かぷりちょうざで"
  const char kKeyWithDe[] = "\xE3\x81\x8B\xE3\x81\xB7\xE3\x82\x8A"
      "\xE3\x81\xA1\xE3\x82\x87\xE3\x81\x86\xE3\x81\x96\xE3\x81\xA7";
  // "カプリチョーザで"
  const char kExpectedSuggestionValueWithDe[] = "\xE3\x82\xAB\xE3\x83\x97"
      "\xE3\x83\xAA\xE3\x83\x81\xE3\x83\xA7\xE3\x83\xBC\xE3\x82\xB6"
      "\xE3\x81\xA7";

  MakeSegmentsForSuggestion(kKeyWithDe, &segments);
  predictor->AggregateRealtimeConversion(
      DictionaryPredictor::REALTIME, *convreq_, &segments, &results);
  EXPECT_EQ(1, results.size());

  EXPECT_EQ(results[0].types, DictionaryPredictor::REALTIME);
  EXPECT_NE(0, (results[0].candidate_attributes &
                Segment::Candidate::SPELLING_CORRECTION));
  EXPECT_EQ(kExpectedSuggestionValueWithDe, results[0].value);
  EXPECT_EQ(1, segments.conversion_segments_size());
}

TEST_F(DictionaryPredictorTest, GetMissSpelledPosition) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  EXPECT_EQ(0, predictor->GetMissSpelledPosition("", ""));

  // EXPECT_EQ(3, predictor->GetMissSpelledPosition(
  //              "れみおめろん",
  //              "レミオロメン"));
  EXPECT_EQ(3, predictor->GetMissSpelledPosition(
      "\xE3\x82\x8C\xE3\x81\xBF\xE3\x81\x8A"
      "\xE3\x82\x81\xE3\x82\x8D\xE3\x82\x93",
      "\xE3\x83\xAC\xE3\x83\x9F\xE3\x82\xAA"
      "\xE3\x83\xAD\xE3\x83\xA1\xE3\x83\xB3"));

  // EXPECT_EQ(5, predictor->GetMissSpelledPosition
  //              "とーとばっく",
  //              "トートバッグ"));
  EXPECT_EQ(5, predictor->GetMissSpelledPosition(
      "\xE3\x81\xA8\xE3\x83\xBC\xE3\x81\xA8\xE3\x81\xB0"
      "\xE3\x81\xA3\xE3\x81\x8F",
      "\xE3\x83\x88\xE3\x83\xBC\xE3\x83\x88\xE3\x83\x90"
      "\xE3\x83\x83\xE3\x82\xB0"));

  // EXPECT_EQ(4, predictor->GetMissSpelledPosition(
  //               "おーすとりらあ",
  //               "オーストラリア"));
  EXPECT_EQ(4, predictor->GetMissSpelledPosition(
      "\xE3\x81\x8A\xE3\x83\xBC\xE3\x81\x99\xE3\x81\xA8"
      "\xE3\x82\x8A\xE3\x82\x89\xE3\x81\x82",
      "\xE3\x82\xAA\xE3\x83\xBC\xE3\x82\xB9\xE3\x83\x88"
      "\xE3\x83\xA9\xE3\x83\xAA\xE3\x82\xA2"));

  // EXPECT_EQ(7, predictor->GetMissSpelledPosition(
  //               "じきそうしょう",
  //               "時期尚早"));
  EXPECT_EQ(7, predictor->GetMissSpelledPosition(
      "\xE3\x81\x98\xE3\x81\x8D\xE3\x81\x9D\xE3\x81\x86"
      "\xE3\x81\x97\xE3\x82\x87\xE3\x81\x86",
      "\xE6\x99\x82\xE6\x9C\x9F\xE5\xB0\x9A\xE6\x97\xA9"));
}

TEST_F(DictionaryPredictorTest, RemoveMissSpelledCandidates) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  {
    vector<DictionaryPredictor::Result> results;
    DictionaryPredictor::Result *result;

    results.push_back(DictionaryPredictor::Result());
    result = &results.back();
    // result->key = "ばっく";
    // rseult->value = "バッグ";
    result->key = "\xE3\x81\xB0\xE3\x81\xA3\xE3\x81\x8F";
    result->value = "\xE3\x83\x90\xE3\x83\x83\xE3\x82\xB0";
    result->SetTypesAndTokenAttributes(DictionaryPredictor::UNIGRAM,
                                       Token::SPELLING_CORRECTION);

    results.push_back(DictionaryPredictor::Result());
    result = &results.back();
    // result->key = "ばっぐ";
    // result->value = "バッグ";
    result->key = "\xE3\x81\xB0\xE3\x81\xA3\xE3\x81\x90";
    result->value = "\xE3\x83\x90\xE3\x83\x83\xE3\x82\xB0";
    result->SetTypesAndTokenAttributes(DictionaryPredictor::UNIGRAM,
                                       Token::NONE);

    results.push_back(DictionaryPredictor::Result());
    result = &results.back();
    // result->key = "ばっく";
    // result->value = "バック";
    result->key = "\xE3\x81\xB0\xE3\x81\xA3\xE3\x81\x8F";
    result->value = "\xE3\x83\x90\xE3\x83\x83\xE3\x82\xAF";
    result->SetTypesAndTokenAttributes(DictionaryPredictor::UNIGRAM,
                                       Token::NONE);

    predictor->RemoveMissSpelledCandidates(1, &results);
    ASSERT_EQ(3, results.size());

    EXPECT_EQ(DictionaryPredictor::NO_PREDICTION,
              results[0].types);
    EXPECT_EQ(DictionaryPredictor::UNIGRAM,
              results[1].types);
    EXPECT_EQ(DictionaryPredictor::NO_PREDICTION,
              results[2].types);
  }

  {
    vector<DictionaryPredictor::Result> results;
    DictionaryPredictor::Result *result;

    results.push_back(DictionaryPredictor::Result());
    result = &results.back();
    // result->key = "ばっく";
    // result->value = "バッグ";
    result->key = "\xE3\x81\xB0\xE3\x81\xA3\xE3\x81\x8F";
    result->value = "\xE3\x83\x90\xE3\x83\x83\xE3\x82\xB0";
    result->SetTypesAndTokenAttributes(DictionaryPredictor::UNIGRAM,
                                       Token::SPELLING_CORRECTION);

    results.push_back(DictionaryPredictor::Result());
    result = &results.back();
    // result->key = "てすと";
    // result->value = "テスト";
    result->key = "\xE3\x81\xA6\xE3\x81\x99\xE3\x81\xA8";
    result->value = "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88";
    result->SetTypesAndTokenAttributes(DictionaryPredictor::UNIGRAM,
                                       Token::NONE);

    predictor->RemoveMissSpelledCandidates(1, &results);
    CHECK_EQ(2, results.size());

    EXPECT_EQ(DictionaryPredictor::UNIGRAM,
              results[0].types);
    EXPECT_EQ(DictionaryPredictor::UNIGRAM,
              results[1].types);
  }

  {
    vector<DictionaryPredictor::Result> results;
    DictionaryPredictor::Result *result;

    results.push_back(DictionaryPredictor::Result());
    result = &results.back();
    // result->key = "ばっく";
    // result->value = "バッグ";
    result->key = "\xE3\x81\xB0\xE3\x81\xA3\xE3\x81\x8F";
    result->value = "\xE3\x83\x90\xE3\x83\x83\xE3\x82\xB0";
    result->SetTypesAndTokenAttributes(DictionaryPredictor::UNIGRAM,
                                       Token::SPELLING_CORRECTION);

    results.push_back(DictionaryPredictor::Result());
    result = &results.back();
    // result->key = "ばっく";
    // result->value = "バック";
    result->key = "\xE3\x81\xB0\xE3\x81\xA3\xE3\x81\x8F";
    result->value = "\xE3\x83\x90\xE3\x83\x83\xE3\x82\xAF";
    result->SetTypesAndTokenAttributes(DictionaryPredictor::UNIGRAM,
                                       Token::NONE);

    predictor->RemoveMissSpelledCandidates(1, &results);
    CHECK_EQ(2, results.size());

    EXPECT_EQ(DictionaryPredictor::NO_PREDICTION,
              results[0].types);
    EXPECT_EQ(DictionaryPredictor::NO_PREDICTION,
              results[1].types);
  }

  {
    vector<DictionaryPredictor::Result> results;
    DictionaryPredictor::Result *result;

    results.push_back(DictionaryPredictor::Result());
    result = &results.back();
    // result->key = "ばっく";
    // result->value = "バッグ";
    result->key = "\xE3\x81\xB0\xE3\x81\xA3\xE3\x81\x8F";
    result->value = "\xE3\x83\x90\xE3\x83\x83\xE3\x82\xB0";
    result->SetTypesAndTokenAttributes(DictionaryPredictor::UNIGRAM,
                                       Token::SPELLING_CORRECTION);

    results.push_back(DictionaryPredictor::Result());
    result = &results.back();
    // result->key = "ばっく";
    // result->value = "バック";
    result->key = "\xE3\x81\xB0\xE3\x81\xA3\xE3\x81\x8F";
    result->value = "\xE3\x83\x90\xE3\x83\x83\xE3\x82\xAF";
    result->SetTypesAndTokenAttributes(DictionaryPredictor::UNIGRAM,
                                       Token::NONE);

    predictor->RemoveMissSpelledCandidates(3, &results);
    CHECK_EQ(2, results.size());

    EXPECT_EQ(DictionaryPredictor::UNIGRAM,
              results[0].types);
    EXPECT_EQ(DictionaryPredictor::NO_PREDICTION,
              results[1].types);
  }
}

TEST_F(DictionaryPredictorTest, UseExpansionForUnigramTest) {
  FLAGS_enable_expansion_for_dictionary_predictor = true;
  ExpansionForUnigramTestHelper(true);
}

TEST_F(DictionaryPredictorTest, UnuseExpansionForUnigramTest) {
  FLAGS_enable_expansion_for_dictionary_predictor = false;
  ExpansionForUnigramTestHelper(false);
}

TEST_F(DictionaryPredictorTest, UseExpansionForBigramTest) {
  FLAGS_enable_expansion_for_dictionary_predictor = true;
  ExpansionForBigramTestHelper(true);
}

TEST_F(DictionaryPredictorTest, UnuseExpansionForBigramTest) {
  FLAGS_enable_expansion_for_dictionary_predictor = false;
  ExpansionForBigramTestHelper(false);
}

TEST_F(DictionaryPredictorTest, UseExpansionForSuffixTest) {
  FLAGS_enable_expansion_for_dictionary_predictor = true;
  ExpansionForSuffixTestHelper(true);
}

TEST_F(DictionaryPredictorTest, UnuseExpansionForSuffixTest) {
  FLAGS_enable_expansion_for_dictionary_predictor = false;
  ExpansionForSuffixTestHelper(false);
}

TEST_F(DictionaryPredictorTest, ExpansionPenaltyForRomanTest) {
  FLAGS_enable_expansion_for_dictionary_predictor = true;
  config_->set_use_dictionary_suggest(true);
  config_->set_use_realtime_conversion(false);

  table_->LoadFromFile("system://romanji-hiragana.tsv");
  composer_->SetTable(table_.get());
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const TestableDictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  Segments segments;
  segments.set_request_type(Segments::PREDICTION);
  InsertInputSequence("ak", composer_.get());
  Segment *segment = segments.add_segment();
  CHECK(segment);
  {
    string query;
    composer_->GetQueryForPrediction(&query);
    segment->set_key(query);
    // "あ"
    EXPECT_EQ("\xe3\x81\x82", query);
  }
  {
    string base;
    set<string> expanded;
    composer_->GetQueriesForPrediction(&base, &expanded);
    // "あ"
    EXPECT_EQ("\xe3\x81\x82", base);
    EXPECT_GT(expanded.size(), 5);
  }

  vector<TestableDictionaryPredictor::Result> results;
  TestableDictionaryPredictor::Result *result;

  results.push_back(TestableDictionaryPredictor::MakeEmptyResult());
  result = &results.back();
  // "あか"
  result->key = "\xe3\x81\x82\xe3\x81\x8b";
  // "赤"
  result->value = "\xe8\xb5\xa4";
  result->SetTypesAndTokenAttributes(TestableDictionaryPredictor::UNIGRAM,
                                     Token::NONE);

  results.push_back(TestableDictionaryPredictor::MakeEmptyResult());
  result = &results.back();
  // "あき"
  result->key = "\xe3\x81\x82\xe3\x81\x8d";
  // "秋"
  result->value = "\xe7\xa7\x8b";
  result->SetTypesAndTokenAttributes(TestableDictionaryPredictor::UNIGRAM,
                                     Token::NONE);

  results.push_back(TestableDictionaryPredictor::MakeEmptyResult());
  result = &results.back();
  // "あかぎ"
  result->key = "\xe3\x81\x82\xe3\x81\x8b\xe3\x81\x8e";
  // "アカギ"
  result->value = "\xe3\x82\xa2\xe3\x82\xab\xe3\x82\xae";
  result->SetTypesAndTokenAttributes(TestableDictionaryPredictor::UNIGRAM,
                                     Token::NONE);

  EXPECT_EQ(3, results.size());
  EXPECT_EQ(0, results[0].cost);
  EXPECT_EQ(0, results[1].cost);
  EXPECT_EQ(0, results[2].cost);

  predictor->ApplyPenaltyForKeyExpansion(segments, &results);

  // no penalties
  EXPECT_EQ(0, results[0].cost);
  EXPECT_EQ(0, results[1].cost);
  EXPECT_EQ(0, results[2].cost);
}

TEST_F(DictionaryPredictorTest, ExpansionPenaltyForKanaTest) {
  FLAGS_enable_expansion_for_dictionary_predictor = true;
  config_->set_use_dictionary_suggest(true);
  config_->set_use_realtime_conversion(false);

  table_->LoadFromFile("system://kana.tsv");
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const TestableDictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  Segments segments;
  segments.set_request_type(Segments::PREDICTION);
  // "あし"
  InsertInputSequence("\xe3\x81\x82\xe3\x81\x97", composer_.get());

  Segment *segment = segments.add_segment();
  CHECK(segment);
  {
    string query;
    composer_->GetQueryForPrediction(&query);
    segment->set_key(query);
    // "あし"
    EXPECT_EQ("\xe3\x81\x82\xe3\x81\x97", query);
  }
  {
    string base;
    set<string> expanded;
    composer_->GetQueriesForPrediction(&base, &expanded);
    // "あ"
    EXPECT_EQ("\xe3\x81\x82", base);
    EXPECT_EQ(2, expanded.size());
  }

  vector<TestableDictionaryPredictor::Result> results;
  TestableDictionaryPredictor::Result *result;

  results.push_back(TestableDictionaryPredictor::MakeEmptyResult());
  result = &results.back();
  // "あし"
  result->key = "\xe3\x81\x82\xe3\x81\x97";
  // "足"
  result->value = "\xe8\xb6\xb3";
  result->SetTypesAndTokenAttributes(TestableDictionaryPredictor::UNIGRAM,
                                     Token::NONE);

  results.push_back(TestableDictionaryPredictor::MakeEmptyResult());
  result = &results.back();
  // "あじ"
  result->key = "\xe3\x81\x82\xe3\x81\x98";
  // "味"
  result->value = "\xe5\x91\xb3";
  result->SetTypesAndTokenAttributes(TestableDictionaryPredictor::UNIGRAM,
                                     Token::NONE);

  results.push_back(TestableDictionaryPredictor::MakeEmptyResult());
  result = &results.back();
  // "あした"
  result->key = "\xe3\x81\x82\xe3\x81\x97\xe3\x81\x9f";
  // "明日"
  result->value = "\xe6\x98\x8e\xe6\x97\xa5";
  result->SetTypesAndTokenAttributes(TestableDictionaryPredictor::UNIGRAM,
                                     Token::NONE);

  results.push_back(TestableDictionaryPredictor::MakeEmptyResult());
  result = &results.back();
  // "あじあ"
  result->key = "\xe3\x81\x82\xe3\x81\x98\xe3\x81\x82";
  // "アジア"
  result->value = "\xe3\x82\xa2\xe3\x82\xb8\xe3\x82\xa2";
  result->SetTypesAndTokenAttributes(TestableDictionaryPredictor::UNIGRAM,
                                     Token::NONE);

  EXPECT_EQ(4, results.size());
  EXPECT_EQ(0, results[0].cost);
  EXPECT_EQ(0, results[1].cost);
  EXPECT_EQ(0, results[2].cost);
  EXPECT_EQ(0, results[3].cost);

  predictor->ApplyPenaltyForKeyExpansion(segments, &results);

  EXPECT_EQ(0, results[0].cost);
  EXPECT_LT(0, results[1].cost);
  EXPECT_EQ(0, results[2].cost);
  EXPECT_LT(0, results[3].cost);
}

TEST_F(DictionaryPredictorTest, SetLMCost) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const TestableDictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  Segments segments;
  segments.set_request_type(Segments::PREDICTION);
  Segment *segment = segments.add_segment();
  CHECK(segment);
  // "てすと"
  segment->set_key("\xe3\x81\xa6\xe3\x81\x99\xe3\x81\xa8");

  vector<TestableDictionaryPredictor::Result> results;
  TestableDictionaryPredictor::Result *result;

  results.push_back(TestableDictionaryPredictor::MakeEmptyResult());
  result = &results.back();
  // "てすと"
  result->key = "\xe3\x81\xa6\xe3\x81\x99\xe3\x81\xa8";
  // "てすと"
  result->value = "\xe3\x81\xa6\xe3\x81\x99\xe3\x81\xa8";
  result->SetTypesAndTokenAttributes(TestableDictionaryPredictor::UNIGRAM,
                                     Token::NONE);

  results.push_back(TestableDictionaryPredictor::MakeEmptyResult());
  result = &results.back();
  // "てすと"
  result->key = "\xe3\x81\xa6\xe3\x81\x99\xe3\x81\xa8";
  // "テスト"
  result->value = "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88";
  result->SetTypesAndTokenAttributes(TestableDictionaryPredictor::UNIGRAM,
                                     Token::NONE);

  results.push_back(TestableDictionaryPredictor::MakeEmptyResult());
  result = &results.back();
  // "てすとてすと"
  result->key = "\xe3\x81\xa6\xe3\x81\x99\xe3\x81\xa8"
      "\xe3\x81\xa6\xe3\x81\x99\xe3\x81\xa8";
  // "テストテスト"
  result->value = "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88"
      "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88";
  result->SetTypesAndTokenAttributes(TestableDictionaryPredictor::UNIGRAM,
                                     Token::NONE);

  predictor->SetLMCost(segments, &results);

  EXPECT_EQ(3, results.size());
  // "てすと"
  EXPECT_EQ("\xe3\x81\xa6\xe3\x81\x99\xe3\x81\xa8", results[0].value);
  // "テスト"
  EXPECT_EQ("\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88", results[1].value);
  // "テストテスト"
  EXPECT_EQ("\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88"
            "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88", results[2].value);
  EXPECT_GT(results[2].cost, results[0].cost);
  EXPECT_GT(results[2].cost, results[1].cost);
}

TEST_F(DictionaryPredictorTest, SuggestSpellingCorrection) {
  testing::MockDataManager data_manager;

  unique_ptr<MockDataAndPredictor> data_and_predictor(
      new MockDataAndPredictor());
  data_and_predictor->Init(CreateSystemDictionaryFromDataManager(data_manager),
                           CreateSuffixDictionaryFromDataManager(data_manager));

  const TestableDictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  Segments segments;
  // "あぼがど"
  MakeSegmentsForPrediction(
      "\xe3\x81\x82\xe3\x81\xbc\xe3\x81\x8c\xe3\x81\xa9", &segments);

  predictor->PredictForRequest(*convreq_, &segments);

  EXPECT_TRUE(FindCandidateByValue(
      segments.conversion_segment(0),
      // "アボカド"
      "\xe3\x82\xa2\xe3\x83\x9c\xe3\x82\xab\xe3\x83\x89"));
}

TEST_F(DictionaryPredictorTest, DoNotSuggestSpellingCorrectionBeforeMismatch) {
  testing::MockDataManager data_manager;

  unique_ptr<MockDataAndPredictor> data_and_predictor(
      new MockDataAndPredictor());
  data_and_predictor->Init(CreateSystemDictionaryFromDataManager(data_manager),
                           CreateSuffixDictionaryFromDataManager(data_manager));

  const TestableDictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  Segments segments;
  // "あぼが"
  MakeSegmentsForPrediction(
      "\xe3\x81\x82\xe3\x81\xbc\xe3\x81\x8c", &segments);

  predictor->PredictForRequest(*convreq_, &segments);

  EXPECT_FALSE(FindCandidateByValue(
      segments.conversion_segment(0),
      // "アボカド"
      "\xe3\x82\xa2\xe3\x83\x9c\xe3\x82\xab\xe3\x83\x89"));
}

TEST_F(DictionaryPredictorTest, MobileUnigramSuggestion) {
  testing::MockDataManager data_manager;

  unique_ptr<MockDataAndPredictor> data_and_predictor(
      new MockDataAndPredictor());
  data_and_predictor->Init(CreateSystemDictionaryFromDataManager(data_manager),
                           CreateSuffixDictionaryFromDataManager(data_manager));

  const TestableDictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  Segments segments;
  // "とうきょう"
  const char kKey[] =
      "\xe3\x81\xa8\xe3\x81\x86\xe3\x81\x8d\xe3\x82\x87\xe3\x81\x86";

  MakeSegmentsForSuggestion(kKey, &segments);

  commands::RequestForUnitTest::FillMobileRequest(request_.get());

  vector<TestableDictionaryPredictor::Result> results;
  predictor->AggregateUnigramPrediction(
      TestableDictionaryPredictor::UNIGRAM,
      *convreq_, segments, &results);

  // "東京"
  EXPECT_TRUE(FindResultByValue(results, "\xe6\x9d\xb1\xe4\xba\xac"));

  int prefix_count = 0;
  for (size_t i = 0; i < results.size(); ++i) {
    // "東京"
    if (Util::StartsWith(results[i].value, "\xe6\x9d\xb1\xe4\xba\xac")) {
      ++prefix_count;
    }
  }
  // Should not have same prefix candidates a lot.
  EXPECT_LE(prefix_count, 6);
}

TEST_F(DictionaryPredictorTest, MobileZeroQuerySuggestion) {
  testing::MockDataManager data_manager;

  unique_ptr<MockDataAndPredictor> data_and_predictor(
      new MockDataAndPredictor());
  data_and_predictor->Init(CreateSystemDictionaryFromDataManager(data_manager),
                           CreateSuffixDictionaryFromDataManager(data_manager));

  const TestableDictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  Segments segments;
  MakeSegmentsForPrediction("", &segments);

  // "だいがく"
  PrependHistorySegments("\xe3\x81\xa0\xe3\x81\x84\xe3\x81\x8c\xe3\x81\x8f",
                         // "大学"
                         "\xe5\xa4\xa7\xe5\xad\xa6",
                         &segments);

  commands::RequestForUnitTest::FillMobileRequest(request_.get());
  predictor->PredictForRequest(*convreq_, &segments);

  EXPECT_TRUE(FindCandidateByValue(segments.conversion_segment(0),
                                   // "入試"
                                   "\xe5\x85\xa5\xe8\xa9\xa6"));
  EXPECT_TRUE(FindCandidateByValue(
      segments.conversion_segment(0),
      // "入試センター"
      "\xe5\x85\xa5\xe8\xa9\xa6\xe3\x82\xbb\xe3\x83\xb3"
      "\xe3\x82\xbf\xe3\x83\xbc"));
}

// We are not sure what should we suggest after the end of sentence for now.
// However, we decided to show zero query suggestion rather than stopping
// zero query completely. Users may be confused if they cannot see suggestion
// window only after the certain conditions.
// TODO(toshiyuki): Show useful zero query suggestions after EOS.
TEST_F(DictionaryPredictorTest, DISABLED_MobileZeroQuerySuggestionAfterEOS) {
  testing::MockDataManager data_manager;

  unique_ptr<MockDataAndPredictor> data_and_predictor(
      new MockDataAndPredictor());
  data_and_predictor->Init(CreateSystemDictionaryFromDataManager(data_manager),
                           CreateSuffixDictionaryFromDataManager(data_manager));

  const TestableDictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  commands::RequestForUnitTest::FillMobileRequest(request_.get());

  const POSMatcher &pos_matcher = data_and_predictor->pos_matcher();

  const struct TestCase {
    const char *key;
    const char *value;
    int rid;
    bool expected_result;
  } kTestcases[] = {
    // "ですよね｡", "ですよね。"
    { "\xe3\x81\xa7\xe3\x81\x99\xe3\x82\x88\xe3\x81\xad\xef\xbd\xa1",
      "\xe3\x81\xa7\xe3\x81\x99\xe3\x82\x88\xe3\x81\xad\xe3\x80\x82",
      pos_matcher.GetEOSSymbolId(),
      false },
    // "｡", "。"
    { "\xef\xbd\xa1",
      "\xe3\x80\x82",
      pos_matcher.GetEOSSymbolId(),
      false },
    // "まるいち", "①"
    { "\xe3\x81\xbe\xe3\x82\x8b\xe3\x81\x84\xe3\x81\xa1",
      "\xe2\x91\xa0",
      pos_matcher.GetEOSSymbolId(),
      false },
    // "そう", "そう"
    { "\xe3\x81\x9d\xe3\x81\x86",
      "\xe3\x81\x9d\xe3\x81\x86",
      pos_matcher.GetGeneralNounId(),
      true },
    // "そう!", "そう！"
    { "\xe3\x81\x9d\xe3\x81\x86\x21",
      "\xe3\x81\x9d\xe3\x81\x86\xef\xbc\x81",
      pos_matcher.GetGeneralNounId(),
      false },
    // "むすめ。", "娘。"
    { "\xe3\x82\x80\xe3\x81\x99\xe3\x82\x81\xe3\x80\x82",
      "\xe5\xa8\x98\xe3\x80\x82",
      pos_matcher.GetUniqueNounId(),
      true },
  };

  for (size_t i = 0; i < arraysize(kTestcases); ++i) {
    const TestCase &test_case = kTestcases[i];

    Segments segments;
    MakeSegmentsForPrediction("", &segments);

    Segment *seg = segments.push_front_segment();
    seg->set_segment_type(Segment::HISTORY);
    seg->set_key(test_case.key);
    Segment::Candidate *c = seg->add_candidate();
    c->key = test_case.key;
    c->content_key = test_case.key;
    c->value = test_case.value;
    c->content_value = test_case.value;
    c->rid = test_case.rid;

    predictor->PredictForRequest(*convreq_, &segments);
    const bool candidates_inserted =
        segments.conversion_segment(0).candidates_size() > 0;
    EXPECT_EQ(test_case.expected_result, candidates_inserted);
  }
}

TEST_F(DictionaryPredictorTest, PropagateUserDictionaryAttribute) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const DictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  Segments segments;
  config_->set_use_dictionary_suggest(true);
  config_->set_use_realtime_conversion(true);

  {
    segments.Clear();
    segments.set_max_prediction_candidates_size(10);
    segments.set_request_type(Segments::SUGGESTION);
    Segment *seg = segments.add_segment();
    // "ゆーざー"
    seg->set_key("\xe3\x82\x86\xe3\x83\xbc\xe3\x81\x96\xe3\x83\xbc");
    seg->set_segment_type(Segment::FREE);
    EXPECT_TRUE(predictor->PredictForRequest(*convreq_,
                                             &segments));
    EXPECT_EQ(1, segments.conversion_segments_size());
    bool find_yuza_candidate = false;
    for (size_t i = 0;
         i < segments.conversion_segment(0).candidates_size();
         ++i) {
      const Segment::Candidate &cand =
          segments.conversion_segment(0).candidate(i);
      // "ユーザー"
      if (cand.value == "\xe3\x83\xa6\xe3\x83\xbc\xe3\x82\xb6\xe3\x83\xbc" &&
          (cand.attributes & (Segment::Candidate::NO_VARIANTS_EXPANSION |
                              Segment::Candidate::USER_DICTIONARY))) {
        find_yuza_candidate = true;
      }
    }
    EXPECT_TRUE(find_yuza_candidate);
  }

  {
    segments.Clear();
    segments.set_max_prediction_candidates_size(10);
    segments.set_request_type(Segments::SUGGESTION);
    Segment *seg = segments.add_segment();
    // "ゆーざーの"
    seg->set_key(
        "\xe3\x82\x86\xe3\x83\xbc\xe3\x81\x96\xe3\x83\xbc\xe3\x81\xae");
    seg->set_segment_type(Segment::FREE);
    EXPECT_TRUE(predictor->PredictForRequest(*convreq_,
                                             &segments));
    EXPECT_EQ(1, segments.conversion_segments_size());
    bool find_yuza_candidate = false;
    for (size_t i = 0;
         i < segments.conversion_segment(0).candidates_size();
         ++i) {
      const Segment::Candidate &cand =
          segments.conversion_segment(0).candidate(i);
      // "ユーザーの"
      if ((cand.value ==
           "\xe3\x83\xa6\xe3\x83\xbc\xe3\x82\xb6\xe3\x83\xbc\xe3\x81\xae") &&
          (cand.attributes & (Segment::Candidate::NO_VARIANTS_EXPANSION |
                              Segment::Candidate::USER_DICTIONARY))) {
        find_yuza_candidate = true;
      }
    }
    EXPECT_TRUE(find_yuza_candidate);
  }
}

TEST_F(DictionaryPredictorTest, SetDescription) {
  {
    string description;
    DictionaryPredictor::SetDescription(
        TestableDictionaryPredictor::TYPING_CORRECTION, 0, &description);
    // "補正"
    EXPECT_EQ("\xE8\xA3\x9C\xE6\xAD\xA3", description);

    description.clear();
    DictionaryPredictor::SetDescription(
        0, Segment::Candidate::AUTO_PARTIAL_SUGGESTION, &description);
    // "部分"
    EXPECT_EQ("\xE9\x83\xA8\xE5\x88\x86", description);
  }
}

TEST_F(DictionaryPredictorTest, SetDebugDescription) {
  {
    string description;
    const TestableDictionaryPredictor::PredictionTypes types =
        TestableDictionaryPredictor::UNIGRAM |
        TestableDictionaryPredictor::ENGLISH;
    DictionaryPredictor::SetDebugDescription(types, &description);
    EXPECT_EQ("UE", description);
  }
  {
    string description = "description";
    const TestableDictionaryPredictor::PredictionTypes types =
        TestableDictionaryPredictor::REALTIME |
        TestableDictionaryPredictor::BIGRAM;
    DictionaryPredictor::SetDebugDescription(types, &description);
    EXPECT_EQ("description BR", description);
  }
  {
    string description;
    const TestableDictionaryPredictor::PredictionTypes types =
        TestableDictionaryPredictor::BIGRAM |
        TestableDictionaryPredictor::REALTIME |
        TestableDictionaryPredictor::SUFFIX;
    DictionaryPredictor::SetDebugDescription(types, &description);
    EXPECT_EQ("BRS", description);
  }
}

TEST_F(DictionaryPredictorTest, PropagateRealtimeConversionBoundary) {
  testing::MockDataManager data_manager;
  unique_ptr<const DictionaryInterface> dictionary(new DictionaryMock);
  unique_ptr<ConverterInterface> converter(new ConverterMock);
  unique_ptr<ImmutableConverterInterface> immutable_converter(
      new ImmutableConverterMock);
  unique_ptr<const DictionaryInterface> suffix_dictionary(
      CreateSuffixDictionaryFromDataManager(data_manager));
  unique_ptr<const Connector> connector(
      Connector::CreateFromDataManager(data_manager));
  unique_ptr<const Segmenter> segmenter(
      Segmenter::CreateFromDataManager(data_manager));
  unique_ptr<const SuggestionFilter> suggestion_filter(
      CreateSuggestionFilter(data_manager));
  unique_ptr<TestableDictionaryPredictor> predictor(
      new TestableDictionaryPredictor(converter.get(),
                                      immutable_converter.get(),
                                      dictionary.get(),
                                      suffix_dictionary.get(),
                                      connector.get(),
                                      segmenter.get(),
                                      data_manager.GetPOSMatcher(),
                                      suggestion_filter.get()));
  Segments segments;
  // "わたしのなまえはなかのです"
  const char kKey[] =
      "\xE3\x82\x8F\xE3\x81\x9F\xE3\x81\x97"
      "\xE3\x81\xAE\xE3\x81\xAA\xE3\x81\xBE"
      "\xE3\x81\x88\xE3\x81\xAF\xE3\x81\xAA"
      "\xE3\x81\x8B\xE3\x81\xAE\xE3\x81\xA7\xE3\x81\x99";
  MakeSegmentsForSuggestion(kKey, &segments);

  vector<TestableDictionaryPredictor::Result> results;
  predictor->AggregateRealtimeConversion(
      TestableDictionaryPredictor::REALTIME, *convreq_,
      &segments, &results);

  // mock results
  EXPECT_EQ(1, results.size());
  predictor->AddPredictionToCandidates(*convreq_,
                                       &segments, &results);
  EXPECT_EQ(1, segments.conversion_segments_size());
  EXPECT_EQ(1, segments.conversion_segment(0).candidates_size());
  const Segment::Candidate &cand = segments.conversion_segment(0).candidate(0);
  // "わたしのなまえはなかのです"
  EXPECT_EQ("\xe3\x82\x8f\xe3\x81\x9f\xe3\x81\x97\xe3\x81\xae\xe3\x81\xaa\xe3"
            "\x81\xbe\xe3\x81\x88\xe3\x81\xaf\xe3\x81\xaa\xe3\x81\x8b\xe3\x81"
            "\xae\xe3\x81\xa7\xe3\x81\x99", cand.key);
  // "私の名前は中野です"
  EXPECT_EQ("\xe7\xa7\x81\xe3\x81\xae\xe5\x90\x8d\xe5\x89\x8d\xe3\x81\xaf\xe4"
            "\xb8\xad\xe9\x87\x8e\xe3\x81\xa7\xe3\x81\x99", cand.value);
  EXPECT_EQ(3, cand.inner_segment_boundary.size());
}

TEST_F(DictionaryPredictorTest, PropagateResultCosts) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const TestableDictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  vector<TestableDictionaryPredictor::Result> results;
  const int kTestSize = 20;
  for (size_t i = 0; i < kTestSize; ++i) {
    results.push_back(TestableDictionaryPredictor::MakeEmptyResult());
    TestableDictionaryPredictor::Result *result = &results.back();
    result->key = string(1, 'a' + i);
    result->value = string(1, 'A' + i);
    result->wcost = i;
    result->cost = i + 1000;
    result->SetTypesAndTokenAttributes(TestableDictionaryPredictor::REALTIME,
                                       Token::NONE);
  }
  std::random_shuffle(results.begin(), results.end());

  Segments segments;
  MakeSegmentsForSuggestion("test", &segments);
  segments.set_max_prediction_candidates_size(kTestSize);

  predictor->AddPredictionToCandidates(*convreq_,
                                       &segments, &results);

  EXPECT_EQ(1, segments.conversion_segments_size());
  ASSERT_EQ(kTestSize, segments.conversion_segment(0).candidates_size());
  const Segment &segment = segments.conversion_segment(0);
  for (size_t i = 0; i < segment.candidates_size(); ++i) {
    EXPECT_EQ(i + 1000, segment.candidate(i).cost);
  }
}

TEST_F(DictionaryPredictorTest, PredictNCandidates) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const TestableDictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  vector<TestableDictionaryPredictor::Result> results;
  const int kTotalCandidateSize = 100;
  const int kLowCostCandidateSize = 5;
  for (size_t i = 0; i < kTotalCandidateSize; ++i) {
    results.push_back(TestableDictionaryPredictor::MakeEmptyResult());
    TestableDictionaryPredictor::Result *result = &results.back();
    result->key = string(1, 'a' + i);
    result->value = string(1, 'A' + i);
    result->wcost = i;
    result->SetTypesAndTokenAttributes(TestableDictionaryPredictor::REALTIME,
                                       Token::NONE);
    if (i < kLowCostCandidateSize) {
      result->cost = i + 1000;
    } else {
      result->cost = i + kInfinity;
    }
  }
  std::random_shuffle(results.begin(), results.end());

  Segments segments;
  MakeSegmentsForSuggestion("test", &segments);
  segments.set_max_prediction_candidates_size(kLowCostCandidateSize + 1);

  predictor->AddPredictionToCandidates(*convreq_,
                                       &segments, &results);

  ASSERT_EQ(1, segments.conversion_segments_size());
  ASSERT_EQ(kLowCostCandidateSize,
            segments.conversion_segment(0).candidates_size());
  const Segment &segment = segments.conversion_segment(0);
  for (size_t i = 0; i < segment.candidates_size(); ++i) {
    EXPECT_EQ(i + 1000, segment.candidate(i).cost);
  }
}

TEST_F(DictionaryPredictorTest, SuggestFilteredwordForExactMatchOnMobile) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const TestableDictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  // turn on mobile mode
  commands::RequestForUnitTest::FillMobileRequest(request_.get());

  Segments segments;
  // Note: The suggestion filter entry "フィルター" for test is not
  // appropriate here, as Katakana entry will be added by realtime conversion.
  // Here, we want to confirm the behavior including unigram prediction.
  // "ふぃるたーたいしょう"
  MakeSegmentsForSuggestion(
      "\xe3\x81\xb5\xe3\x81\x83\xe3\x82\x8b\xe3\x81\x9f\xe3\x83\xbc"
      "\xe3\x81\x9f\xe3\x81\x84\xe3\x81\x97\xe3\x82\x87\xe3\x81\x86",
      &segments);

  EXPECT_TRUE(predictor->PredictForRequest(*convreq_, &segments));
  // "フィルター対象"
  EXPECT_TRUE(
      FindCandidateByValue(segments.conversion_segment(0),
                           "\xe3\x83\x95\xe3\x82\xa3\xe3\x83\xab\xe3\x82"
                           "\xbf\xe3\x83\xbc\xe5\xaf\xbe\xe8\xb1\xa1"));
  EXPECT_TRUE(
      FindCandidateByValue(segments.conversion_segment(0),
                           // "フィルター大将"
                           "\xe3\x83\x95\xe3\x82\xa3\xe3\x83\xab\xe3\x82\xbf"
                           "\xe3\x83\xbc\xe5\xa4\xa7\xe5\xb0\x86"));

  // However, filtered word should not be the top.
  // "フィルター大将"
  EXPECT_EQ("\xe3\x83\x95\xe3\x82\xa3\xe3\x83\xab\xe3\x82\xbf\xe3\x83\xbc"
            "\xe5\xa4\xa7\xe5\xb0\x86",
            segments.conversion_segment(0).candidate(0).value);

  // Should not be there for non-exact suggestion.
  MakeSegmentsForSuggestion(
      // "ふぃるたーたいし"
      "\xe3\x81\xb5\xe3\x81\x83\xe3\x82\x8b\xe3\x81\x9f\xe3\x83\xbc"
      "\xe3\x81\x9f\xe3\x81\x84\xe3\x81\x97",
      &segments);
  EXPECT_TRUE(predictor->PredictForRequest(*convreq_, &segments));
  // "フィルター対象"
  EXPECT_FALSE(
      FindCandidateByValue(segments.conversion_segment(0),
                           "\xe3\x83\x95\xe3\x82\xa3\xe3\x83\xab\xe3\x82"
                           "\xbf\xe3\x83\xbc\xe5\xaf\xbe\xe8\xb1\xa1"));
}

TEST_F(DictionaryPredictorTest, SuppressFilteredwordForExactMatch) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  const TestableDictionaryPredictor *predictor =
      data_and_predictor->dictionary_predictor();

  Segments segments;
  // Note: The suggestion filter entry "フィルター" for test is not
  // appropriate here, as Katakana entry will be added by realtime conversion.
  // Here, we want to confirm the behavior including unigram prediction.
  // "ふぃるたーたいしょう"
  MakeSegmentsForSuggestion(
      "\xe3\x81\xb5\xe3\x81\x83\xe3\x82\x8b\xe3\x81\x9f\xe3\x83\xbc"
      "\xe3\x81\x9f\xe3\x81\x84\xe3\x81\x97\xe3\x82\x87\xe3\x81\x86",
      &segments);

  EXPECT_TRUE(predictor->PredictForRequest(
      *convreq_, &segments));
  // "フィルター対象"
  EXPECT_FALSE(
      FindCandidateByValue(segments.conversion_segment(0),
                           "\xe3\x83\x95\xe3\x82\xa3\xe3\x83\xab\xe3\x82"
                           "\xbf\xe3\x83\xbc\xe5\xaf\xbe\xe8\xb1\xa1"));
}

namespace {
const char *kTestKey0 = "\xe3\x81\x82";  // "あ"
const ZeroQueryEntry kTestValues0[] = {
  // emoji exclamation
  {ZERO_QUERY_EMOJI, "", EMOJI_DOCOMO | EMOJI_SOFTBANK, 0xfeb04},
  {ZERO_QUERY_EMOJI, "\xE2\x9D\x95", EMOJI_UNICODE, 0xfeb0b},  // "❕"
  {ZERO_QUERY_NONE, "\xE2\x9D\xA3", EMOJI_NONE, 0x0},  // "❣"
};
const char *kTestKey1 = "\xe3\x81\x82\xe3\x81\x82";  // "ああ"
const ZeroQueryEntry kTestValues1[] = {
  // "( •̀ㅁ•́;)"
  {
    ZERO_QUERY_EMOTICON,
    "\x28\x20\xE2\x80\xA2\xCC\x80\xE3\x85\x81\xE2\x80\xA2\xCC\x81\x3B\x29",
    EMOJI_NONE, 0x0
  },
};
const ZeroQueryList kTestData_data[] = {
  {kTestKey0, kTestValues0, 3},
  {kTestKey1, kTestValues1, 1},
};
const size_t kTestData_size = 2;

struct TestEntry {
  int32 available_emoji_carrier;
  string key;
  bool expected_result;
  // candidate value and ZeroQueryType.
  vector<string> expected_candidates;
  vector<int32> expected_types;

  string DebugString() const {
    string candidates;
    Util::JoinStrings(expected_candidates, ", ", &candidates);
    string types;
    for (size_t i = 0; i < expected_types.size(); ++i) {
      if (i != 0) {
        types.append(", ");
      }
      types.append(Util::StringPrintf("%d", types[i]));
    }
    return Util::StringPrintf(
        "available_emoji_carrier: %d\n"
        "key: %s\n"
        "expected_result: %d\n"
        "expected_candidates: %s\n"
        "expected_types: %s",
        available_emoji_carrier,
        key.c_str(),
        expected_result,
        candidates.c_str(),
        types.c_str());
  }
};
}  // namespace

TEST_F(DictionaryPredictorTest, GetZeroQueryCandidates) {
  vector<TestEntry> test_entries;
  {
    TestEntry entry;
    entry.available_emoji_carrier = 0;
    entry.key = "a";
    entry.expected_result = false;
    entry.expected_candidates.clear();
    entry.expected_types.clear();
    test_entries.push_back(entry);
  }
  {
    TestEntry entry;
    entry.available_emoji_carrier = 0;
    entry.key = "\xe3\x82\x93";  // "ん"
    entry.expected_result = false;
    entry.expected_candidates.clear();
    entry.expected_types.clear();
    test_entries.push_back(entry);
  }
  {
    TestEntry entry;
    entry.available_emoji_carrier = 0;
    entry.key = "\xe3\x81\x82\xe3\x81\x82";  // "ああ"
    entry.expected_result = true;
    entry.expected_candidates.push_back(
        // "( •̀ㅁ•́;)"
        "\x28\x20\xE2\x80\xA2\xCC\x80\xE3\x85\x81\xE2\x80\xA2\xCC\x81\x3B\x29");
    entry.expected_types.push_back(ZERO_QUERY_EMOTICON);
    test_entries.push_back(entry);
  }
  {
    TestEntry entry;
    entry.available_emoji_carrier = 0;
    entry.key = "\xe3\x81\x82";  // "あ"
    entry.expected_result = true;
    entry.expected_candidates.push_back("\xE2\x9D\xA3");   // "❣"
    entry.expected_types.push_back(ZERO_QUERY_NONE);
    test_entries.push_back(entry);
  }
  {
    TestEntry entry;
    entry.available_emoji_carrier = commands::Request::UNICODE_EMOJI;
    entry.key = "\xe3\x81\x82";  // "あ"
    entry.expected_result = true;
    entry.expected_candidates.push_back("\xE2\x9D\x95");   // "❕"
    entry.expected_types.push_back(ZERO_QUERY_EMOJI);

    entry.expected_candidates.push_back("\xE2\x9D\xA3");   // "❣"
    entry.expected_types.push_back(ZERO_QUERY_NONE);
    test_entries.push_back(entry);
  }
  {
    TestEntry entry;
    entry.available_emoji_carrier = commands::Request::DOCOMO_EMOJI;
    entry.key = "\xe3\x81\x82";  // "あ"
    entry.expected_result = true;
    string candidate;
    Util::UCS4ToUTF8(0xfeb04, &candidate);  // exclamation
    entry.expected_candidates.push_back(candidate);
    entry.expected_types.push_back(ZERO_QUERY_EMOJI);

    entry.expected_candidates.push_back("\xE2\x9D\xA3");   // "❣"
    entry.expected_types.push_back(ZERO_QUERY_NONE);
    test_entries.push_back(entry);
  }
  {
    TestEntry entry;
    entry.available_emoji_carrier = commands::Request::KDDI_EMOJI;
    entry.key = "\xe3\x81\x82";  // "あ"
    entry.expected_result = true;
    entry.expected_candidates.push_back("\xE2\x9D\xA3");   // "❣"
    entry.expected_types.push_back(ZERO_QUERY_NONE);
    test_entries.push_back(entry);
  }
  {
    TestEntry entry;
    entry.available_emoji_carrier =
        (commands::Request::DOCOMO_EMOJI |
         commands::Request::SOFTBANK_EMOJI |
         commands::Request::UNICODE_EMOJI);
    entry.key = "\xe3\x81\x82";  // "あ"
    entry.expected_result = true;
    string candidate;
    Util::UCS4ToUTF8(0xfeb04, &candidate);  // exclamation
    entry.expected_candidates.push_back(candidate);
    entry.expected_types.push_back(ZERO_QUERY_EMOJI);

    entry.expected_candidates.push_back("\xE2\x9D\x95");   // "❕"
    entry.expected_types.push_back(ZERO_QUERY_EMOJI);

    entry.expected_candidates.push_back("\xE2\x9D\xA3");   // "❣"
    entry.expected_types.push_back(ZERO_QUERY_NONE);
    test_entries.push_back(entry);
  }

  for (size_t i = 0; i < test_entries.size(); ++i) {
    const TestEntry &test_entry = test_entries[i];
    ASSERT_EQ(test_entry.expected_candidates.size(),
              test_entry.expected_types.size());

    commands::Request client_request;
    client_request.set_available_emoji_carrier(
        test_entry.available_emoji_carrier);
    composer::Table table;
    const config::Config &config = config::ConfigHandler::DefaultConfig();
    composer::Composer composer(&table, &client_request, &config);
    const ConversionRequest request(&composer, &client_request, &config);

    vector<DictionaryPredictor::ZeroQueryResult> actual_candidates;
    const bool actual_result =
        DictionaryPredictor::GetZeroQueryCandidatesForKey(
            request, test_entry.key,
            kTestData_data, kTestData_data + kTestData_size,
            &actual_candidates);
    EXPECT_EQ(test_entry.expected_result, actual_result)
        << test_entry.DebugString();
    for (size_t j = 0; j < test_entry.expected_candidates.size(); ++j) {
      EXPECT_EQ(test_entry.expected_candidates[j], actual_candidates[j].first)
          << "Failed at " << j << " : " << test_entry.DebugString();
      EXPECT_EQ(test_entry.expected_types[j], actual_candidates[j].second)
          << "Failed at " << j << " : " << test_entry.DebugString();
    }
  }
}

namespace {
void SetSegmentForCommit(const string &candidate_value,
                         int candidate_source_info, Segments *segments) {
  segments->Clear();
  Segment *segment = segments->add_segment();
  segment->set_key("");
  segment->set_segment_type(Segment::FIXED_VALUE);
  Segment::Candidate *candidate = segment->add_candidate();
  candidate->key = candidate_value;
  candidate->content_key = candidate_value;
  candidate->value = candidate_value;
  candidate->content_value = candidate_value;
  candidate->source_info = candidate_source_info;
}
}  // namespace

TEST_F(DictionaryPredictorTest, UsageStats) {
  unique_ptr<MockDataAndPredictor> data_and_predictor(
      CreateDictionaryPredictorWithMockData());
  DictionaryPredictor *predictor =
      data_and_predictor->mutable_dictionary_predictor();

  Segments segments;
  EXPECT_COUNT_STATS("CommitDictionaryPredictorZeroQueryTypeNone", 0);
  SetSegmentForCommit(
      // "★"
      "\xe2\x98\x85",
      Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_NONE,
      &segments);
  predictor->Finish(*convreq_, &segments);
  EXPECT_COUNT_STATS("CommitDictionaryPredictorZeroQueryTypeNone", 1);

  EXPECT_COUNT_STATS("CommitDictionaryPredictorZeroQueryTypeNumberSuffix", 0);
  SetSegmentForCommit(
      // "個"
      "\xe5\x80\x8b",
      Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_NUMBER_SUFFIX,
      &segments);
  predictor->Finish(*convreq_, &segments);
  EXPECT_COUNT_STATS("CommitDictionaryPredictorZeroQueryTypeNumberSuffix", 1);

  EXPECT_COUNT_STATS("CommitDictionaryPredictorZeroQueryTypeEmoticon", 0);
  SetSegmentForCommit(
      // "＼(^o^)／"
      "\xef\xbc\xbc\x28\x5e\x6f\x5e\x29\xef\xbc\x8f",
      Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_EMOTICON,
      &segments);
  predictor->Finish(*convreq_, &segments);
  EXPECT_COUNT_STATS("CommitDictionaryPredictorZeroQueryTypeEmoticon", 1);

  EXPECT_COUNT_STATS("CommitDictionaryPredictorZeroQueryTypeEmoji", 0);
  SetSegmentForCommit(
      // "❕"
      "\xe2\x9d\x95",
      Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_EMOJI,
      &segments);
  predictor->Finish(*convreq_, &segments);
  EXPECT_COUNT_STATS("CommitDictionaryPredictorZeroQueryTypeEmoji", 1);

  EXPECT_COUNT_STATS("CommitDictionaryPredictorZeroQueryTypeBigram", 0);
  SetSegmentForCommit(
      // "ヒルズ"
      "\xe3\x83\x92\xe3\x83\xab\xe3\x82\xba",
      Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_BIGRAM,
      &segments);
  predictor->Finish(*convreq_, &segments);
  EXPECT_COUNT_STATS("CommitDictionaryPredictorZeroQueryTypeBigram", 1);

  EXPECT_COUNT_STATS("CommitDictionaryPredictorZeroQueryTypeSuffix", 0);
  SetSegmentForCommit(
      // "が"
      "\xe3\x81\x8c",
      Segment::Candidate::DICTIONARY_PREDICTOR_ZERO_QUERY_SUFFIX,
      &segments);
  predictor->Finish(*convreq_, &segments);
  EXPECT_COUNT_STATS("CommitDictionaryPredictorZeroQueryTypeSuffix", 1);
}

}  // namespace mozc
